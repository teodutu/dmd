/**
 * Compiler implementation of the
 * $(LINK2 http://www.dlang.org, D programming language).
 *
 * Copyright:   Copyright (C) 2009-2018 by The D Language Foundation, All Rights Reserved
 * Authors:     $(LINK2 http://www.digitalmars.com, Walter Bright)
 * License:     $(LINK2 http://www.boost.org/LICENSE_1_0.txt, Boost License 1.0)
 * Source:      $(LINK2 https://github.com/dlang/dmd/blob/master/src/dmd/backend/machobj.c, backend/machobj.c)
 */


#if SCPP || MARS
#include        <stdio.h>
#include        <string.h>
#include        <stdlib.h>
#include        <sys/types.h>
#include        <sys/stat.h>
#include        <fcntl.h>
#include        <ctype.h>

#if _WIN32 || __linux__
#include        <malloc.h>
#endif

#if __linux__ || __APPLE__ || __FreeBSD__ || __OpenBSD__ || __sun
#include        <signal.h>
#include        <unistd.h>
#endif

#include        "cc.h"
#include        "global.h"
#include        "code.h"
#include        "type.h"
#include        "mach.h"
#include        "outbuf.h"
#include        "filespec.h"
#include        "cv4.h"
#include        "cgcv.h"
#include        "dt.h"

#include        "aa.h"
#include        "tinfo.h"

#if MACHOBJ

#if MARS
#include        "mars.h"
#endif

#include        "mach.h"
#include        "dwarf.h"

// for x86_64
enum
{
    X86_64_RELOC_UNSIGNED         = 0,
    X86_64_RELOC_SIGNED           = 1,
    X86_64_RELOC_BRANCH           = 2,
    X86_64_RELOC_GOT_LOAD         = 3,
    X86_64_RELOC_GOT              = 4,
    X86_64_RELOC_SUBTRACTOR       = 5,
    X86_64_RELOC_SIGNED_1         = 6,
    X86_64_RELOC_SIGNED_2         = 7,
    X86_64_RELOC_SIGNED_4         = 8,
    X86_64_RELOC_TLV              = 9, // for thread local variables
};

static Outbuffer *fobjbuf;

static char __file__[] = __FILE__;      // for tassert.h
#include        "tassert.h"

#define DEST_LEN (IDMAX + IDOHD + 1)
char *obj_mangle2(Symbol *s,char *dest);

#if MARS
// C++ name mangling is handled by front end
#define cpp_mangle(s) ((s)->Sident)
#endif

extern int except_table_seg;        // segment of __gcc_except_tab
extern int eh_frame_seg;            // segment of __eh_frame


/******************************************
 */

symbol *GOTsym; // global offset table reference
symbol *tlv_bootstrap_sym;

symbol *Obj::getGOTsym()
{
    if (!GOTsym)
    {
        GOTsym = symbol_name("_GLOBAL_OFFSET_TABLE_",SCglobal,tspvoid);
    }
    return GOTsym;
}

static void objfile_write(FILE *fd, void *buffer, unsigned len);

STATIC char * objmodtoseg (const char *modname);
STATIC void objfixupp (struct FIXUP *);
STATIC void ledata_new (int seg,targ_size_t offset);

static long elf_align(targ_size_t size, long offset);

// The object file is built is several separate pieces


// String Table  - String table for all other names
static Outbuffer *symtab_strings;

// Section Headers
Outbuffer  *SECbuf;             // Buffer to build section table in
#define SecHdrTab   ((struct section *)SECbuf->buf)
#define SecHdrTab64 ((struct section_64 *)SECbuf->buf)

// The relocation for text and data seems to get lost.
// Try matching the order gcc output them
// This means defining the sections and then removing them if they are
// not used.
static int section_cnt;         // Number of sections in table
#define SEC_TAB_INIT 16         // Initial number of sections in buffer
#define SEC_TAB_INC  4          // Number of sections to increment buffer by

#define SYM_TAB_INIT 100        // Initial number of symbol entries in buffer
#define SYM_TAB_INC  50         // Number of symbols to increment buffer by

/* Three symbol tables, because the different types of symbols
 * are grouped into 3 different types (and a 4th for comdef's).
 */

static Outbuffer *local_symbuf;
static Outbuffer *public_symbuf;
static Outbuffer *extern_symbuf;

static void reset_symbols(Outbuffer *buf)
{
    symbol **p = (symbol **)buf->buf;
    const size_t n = buf->size() / sizeof(symbol *);
    for (size_t i = 0; i < n; ++i)
        symbol_reset(p[i]);
}

struct Comdef { symbol *sym; targ_size_t size; int count; };
static Outbuffer *comdef_symbuf;        // Comdef's are stored here

static Outbuffer *indirectsymbuf1;      // indirect symbol table of Symbol*'s
static int jumpTableSeg;                // segment index for __jump_table

static Outbuffer *indirectsymbuf2;      // indirect symbol table of Symbol*'s
static int pointersSeg;                 // segment index for __pointers

/* If an Obj::external_def() happens, set this to the string index,
 * to be added last to the symbol table.
 * Obviously, there can be only one.
 */
static IDXSTR extdef;

#if 0
enum
{
    STI_FILE  = 1,            // Where file symbol table entry is
    STI_TEXT  = 2,
    STI_DATA  = 3,
    STI_BSS   = 4,
    STI_GCC   = 5,            // Where "gcc2_compiled" symbol is */
    STI_RODAT = 6,            // Symbol for readonly data
    STI_COM   = 8,
};
#endif

// Each compiler segment is a section
// Predefined compiler segments CODE,DATA,CDATA,UDATA map to indexes
//      into SegData[]
//      New compiler segments are added to end.

/******************************
 * Returns !=0 if this segment is a code segment.
 */

int seg_data::isCode()
{
    // The codegen assumes that code->data references are indirect,
    // but when CDATA is treated as code reftoident will emit a direct
    // relocation.
    if (this == SegData[CDATA])
        return false;

    if (I64)
    {
        //printf("SDshtidx = %d, x%x\n", SDshtidx, SecHdrTab64[SDshtidx].flags);
        return strcmp(SecHdrTab64[SDshtidx].segname, "__TEXT") == 0;
    }
    else
    {
        //printf("SDshtidx = %d, x%x\n", SDshtidx, SecHdrTab[SDshtidx].flags);
        return strcmp(SecHdrTab[SDshtidx].segname, "__TEXT") == 0;
    }
}


seg_data **SegData;
int seg_count;
int seg_max;

/**
 * Section index for the __thread_vars/__tls_data section.
 *
 * This section is used for the variable symbol for TLS variables.
 */
int seg_tlsseg = UNKNOWN;

/**
 * Section index for the __thread_bss section.
 *
 * This section is used for the data symbol ($tlv$init) for TLS variables
 * without an initializer.
 */
int seg_tlsseg_bss = UNKNOWN;

/**
 * Section index for the __thread_data section.
 *
 * This section is used for the data symbol ($tlv$init) for TLS variables
 * with an initializer.
 */
int seg_tlsseg_data = UNKNOWN;

/*******************************************************
 * Because the Mach-O relocations cannot be computed until after
 * all the segments are written out, and we need more information
 * than the Mach-O relocations provide, make our own relocation
 * type. Later, translate to Mach-O relocation structure.
 */

enum
{
    RELaddr = 0,      // straight address
    RELrel  = 1,      // relative to location to be fixed up
};

struct Relocation
{   // Relocations are attached to the struct seg_data they refer to
    targ_size_t offset; // location in segment to be fixed up
    symbol *funcsym;    // function in which offset lies, if any
    symbol *targsym;    // if !=NULL, then location is to be fixed up
                        // to address of this symbol
    unsigned targseg;   // if !=0, then location is to be fixed up
                        // to address of start of this segment
    unsigned char rtype;   // RELxxxx
    unsigned char flag; // 1: emit SUBTRACTOR/UNSIGNED pair
    short val;          // 0, -1, -2, -4
};


/*******************************
 * Output a string into a string table
 * Input:
 *      strtab  =       string table for entry
 *      str     =       string to add
 *
 * Returns index into the specified string table.
 */

IDXSTR Obj::addstr(Outbuffer *strtab, const char *str)
{
    //printf("Obj::addstr(strtab = %p str = '%s')\n",strtab,str);
    IDXSTR idx = strtab->size();        // remember starting offset
    strtab->writeString(str);
    //printf("\tidx %d, new size %d\n",idx,strtab->size());
    return idx;
}

/*******************************
 * Find a string in a string table
 * Input:
 *      strtab  =       string table for entry
 *      str     =       string to find
 *
 * Returns index into the specified string table or 0.
 */

static IDXSTR elf_findstr(Outbuffer *strtab, const char *str, const char *suffix)
{
    const char *ent = (char *)strtab->buf+1;
    const char *pend = ent+strtab->size() - 1;
    const char *s = str;
    const char *sx = suffix;
    int len = strlen(str);

    if (suffix)
        len += strlen(suffix);

    while(ent < pend)
    {
        if(*ent == 0)                   // end of table entry
        {
            if(*s == 0 && !sx)          // end of string - found a match
            {
                return ent - (const char *)strtab->buf - len;
            }
            else                        // table entry too short
            {
                s = str;                // back to beginning of string
                sx = suffix;
                ent++;                  // start of next table entry
            }
        }
        else if (*s == 0 && sx && *sx == *ent)
        {                               // matched first string
            s = sx+1;                   // switch to suffix
            ent++;
            sx = NULL;
        }
        else                            // continue comparing
        {
            if (*ent == *s)
            {                           // Have a match going
                ent++;
                s++;
            }
            else                        // no match
            {
                while(*ent != 0)        // skip to end of entry
                    ent++;
                ent++;                  // start of next table entry
                s = str;                // back to beginning of string
                sx = suffix;
            }
        }
    }
    return 0;                   // never found match
}

/*******************************
 * Output a mangled string into the symbol string table
 * Input:
 *      str     =       string to add
 *
 * Returns index into the table.
 */

static IDXSTR elf_addmangled(Symbol *s)
{
    //printf("elf_addmangled(%s)\n", s->Sident);
    char dest[DEST_LEN];
    char *destr;
    const char *name;
    int len;
    IDXSTR namidx;

    namidx = symtab_strings->size();
    destr = obj_mangle2(s, dest);
    name = destr;
    if (CPP && name[0] == '_' && name[1] == '_')
    {
        if (strncmp(name,"__ct__",6) == 0)
            name += 4;
#if 0
        switch(name[2])
        {
            case 'c':
                if (strncmp(name,"__ct__",6) == 0)
                    name += 4;
                break;
            case 'd':
                if (strcmp(name,"__dl__FvP") == 0)
                    name = "__builtin_delete";
                break;
            case 'v':
                //if (strcmp(name,"__vec_delete__FvPiUIPi") == 0)
                    //name = "__builtin_vec_del";
                //else
                //if (strcmp(name,"__vn__FPUI") == 0)
                    //name = "__builtin_vec_new";
                break;
            case 'n':
                if (strcmp(name,"__nw__FPUI") == 0)
                    name = "__builtin_new";
                break;
        }
#endif
    }
    else if (tyfunc(s->ty()) && s->Sfunc && s->Sfunc->Fredirect)
        name = s->Sfunc->Fredirect;
    len = strlen(name);
    symtab_strings->reserve(len+1);
    strcpy((char *)symtab_strings->p,name);
    symtab_strings->setsize(namidx+len+1);
    if (destr != dest)                  // if we resized result
        mem_free(destr);
    //dbg_printf("\telf_addmagled symtab_strings %s namidx %d len %d size %d\n",name, namidx,len,symtab_strings->size());
    return namidx;
}

/**************************
 * Ouput read only data and generate a symbol for it.
 *
 */

symbol * Obj::sym_cdata(tym_t ty,char *p,int len)
{
    symbol *s;

#if 0
    if (I64)
    {
        alignOffset(DATA, tysize(ty));
        s = symboldata(Offset(DATA), ty);
        SegData[DATA]->SDbuf->write(p,len);
        s->Sseg = DATA;
        s->Soffset = Offset(DATA);   // Remember its offset into DATA section
        Offset(DATA) += len;
    }
    else
#endif
    {
        //printf("Obj::sym_cdata(ty = %x, p = %x, len = %d, Offset(CDATA) = %x)\n", ty, p, len, Offset(CDATA));
        alignOffset(CDATA, tysize(ty));
        s = symboldata(Offset(CDATA), ty);
        s->Sseg = CDATA;
        //Obj::pubdef(CDATA, s, Offset(CDATA));
        Obj::bytes(CDATA, Offset(CDATA), len, p);
    }

    s->Sfl = /*(config.flags3 & CFG3pic) ? FLgotoff :*/ FLextern;
    return s;
}

/**************************
 * Ouput read only data for data
 *
 */

int Obj::data_readonly(char *p, int len, int *pseg)
{
    int oldoff = Offset(CDATA);
    SegData[CDATA]->SDbuf->reserve(len);
    SegData[CDATA]->SDbuf->writen(p,len);
    Offset(CDATA) += len;
    *pseg = CDATA;
    return oldoff;
}

int Obj::data_readonly(char *p, int len)
{
    int pseg;

    return Obj::data_readonly(p, len, &pseg);
}

/*****************************
 * Get segment for readonly string literals.
 * The linker will pool strings in this section.
 * Params:
 *    sz = number of bytes per character (1, 2, or 4)
 * Returns:
 *    segment index
 */
int Obj::string_literal_segment(unsigned sz)
{
    if (sz == 1)
    {
        return MachObj::getsegment("__cstring", "__TEXT", 0, S_CSTRING_LITERALS);
    }
    return CDATA;  // no special handling for other wstring, dstring; use __const
}

/******************************
 * Perform initialization that applies to all .o output files.
 *      Called before any other obj_xxx routines
 */

Obj *Obj::init(Outbuffer *objbuf, const char *filename, const char *csegname)
{
    //printf("Obj::init()\n");
    MachObj *obj = I64 ? new MachObj64() : new MachObj();

    cseg = CODE;
    fobjbuf = objbuf;

    seg_tlsseg = UNKNOWN;
    seg_tlsseg_bss = UNKNOWN;
    seg_tlsseg_data = UNKNOWN;
    GOTsym = NULL;
    tlv_bootstrap_sym = NULL;

    // Initialize buffers

    if (symtab_strings)
        symtab_strings->setsize(1);
    else
    {   symtab_strings = new Outbuffer(1024);
        symtab_strings->reserve(2048);
        symtab_strings->writeByte(0);
    }

    if (!local_symbuf)
        local_symbuf = new Outbuffer(sizeof(symbol *) * SYM_TAB_INIT);
    local_symbuf->setsize(0);

    if (public_symbuf)
    {
        reset_symbols(public_symbuf);
        public_symbuf->setsize(0);
    }
    else
        public_symbuf = new Outbuffer(sizeof(symbol *) * SYM_TAB_INIT);

    if (extern_symbuf)
    {
        reset_symbols(extern_symbuf);
        extern_symbuf->setsize(0);
    }
    else
        extern_symbuf = new Outbuffer(sizeof(symbol *) * SYM_TAB_INIT);

    if (!comdef_symbuf)
        comdef_symbuf = new Outbuffer(sizeof(symbol *) * SYM_TAB_INIT);
    comdef_symbuf->setsize(0);

    extdef = 0;

    if (indirectsymbuf1)
        indirectsymbuf1->setsize(0);
    jumpTableSeg = 0;

    if (indirectsymbuf2)
        indirectsymbuf2->setsize(0);
    pointersSeg = 0;

    // Initialize segments for CODE, DATA, UDATA and CDATA
    size_t struct_section_size = I64 ? sizeof(struct section_64) : sizeof(struct section);
    if (SECbuf)
    {
        SECbuf->setsize(struct_section_size);
    }
    else
    {
        SECbuf = new Outbuffer(SYM_TAB_INC * struct_section_size);
        SECbuf->reserve(SEC_TAB_INIT * struct_section_size);
        // Ignore the first section - section numbers start at 1
        SECbuf->writezeros(struct_section_size);
    }
    section_cnt = 1;

    seg_count = 0;
    int align = I64 ? 4 : 2;            // align to 16 bytes for floating point
    MachObj::getsegment("__text",  "__TEXT", 2, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    MachObj::getsegment("__data",  "__DATA", align, S_REGULAR);     // DATA
    MachObj::getsegment("__const", "__TEXT", 2, S_REGULAR);         // CDATA
    MachObj::getsegment("__bss",   "__DATA", 4, S_ZEROFILL);        // UDATA
    MachObj::getsegment("__const", "__DATA", align, S_REGULAR);     // CDATAREL

    dwarf_initfile(filename);
    return obj;
}

/**************************
 * Initialize the start of object output for this particular .o file.
 *
 * Input:
 *      filename:       Name of source file
 *      csegname:       User specified default code segment name
 */

void Obj::initfile(const char *filename, const char *csegname, const char *modname)
{
    //dbg_printf("Obj::initfile(filename = %s, modname = %s)\n",filename,modname);
#if SCPP
    if (csegname && *csegname && strcmp(csegname,".text"))
    {   // Define new section and make it the default for cseg segment
        // NOTE: cseg is initialized to CODE
        IDXSEC newsecidx;
        Elf32_Shdr *newtextsec;
        IDXSYM newsymidx;
        assert(!I64);      // fix later
        SegData[cseg]->SDshtidx = newsecidx =
            elf_newsection(csegname,0,SHT_PROGDEF,SHF_ALLOC|SHF_EXECINSTR);
        newtextsec = &SecHdrTab[newsecidx];
        newtextsec->sh_addralign = 4;
        SegData[cseg]->SDsymidx =
            elf_addsym(0, 0, 0, STT_SECTION, STB_LOCAL, newsecidx);
    }
#endif
    if (config.fulltypes)
        dwarf_initmodule(filename, modname);
}

/************************************
 * Patch pseg/offset by adding in the vmaddr difference from
 * pseg/offset to start of seg.
 */

int32_t *patchAddr(int seg, targ_size_t offset)
{
    return(int32_t *)(fobjbuf->buf + SecHdrTab[SegData[seg]->SDshtidx].offset + offset);
}

int32_t *patchAddr64(int seg, targ_size_t offset)
{
    return(int32_t *)(fobjbuf->buf + SecHdrTab64[SegData[seg]->SDshtidx].offset + offset);
}

void patch(seg_data *pseg, targ_size_t offset, int seg, targ_size_t value)
{
    //printf("patch(offset = x%04x, seg = %d, value = x%llx)\n", (unsigned)offset, seg, value);
    if (I64)
    {
        int32_t *p = (int32_t *)(fobjbuf->buf + SecHdrTab64[pseg->SDshtidx].offset + offset);
#if 0
        printf("\taddr1 = x%llx\n\taddr2 = x%llx\n\t*p = x%llx\n\tdelta = x%llx\n",
            SecHdrTab64[pseg->SDshtidx].addr,
            SecHdrTab64[SegData[seg]->SDshtidx].addr,
            *p,
            SecHdrTab64[SegData[seg]->SDshtidx].addr -
            (SecHdrTab64[pseg->SDshtidx].addr + offset));
#endif
        *p += SecHdrTab64[SegData[seg]->SDshtidx].addr -
              (SecHdrTab64[pseg->SDshtidx].addr - value);
    }
    else
    {
        int32_t *p = (int32_t *)(fobjbuf->buf + SecHdrTab[pseg->SDshtidx].offset + offset);
#if 0
        printf("\taddr1 = x%x\n\taddr2 = x%x\n\t*p = x%x\n\tdelta = x%x\n",
            SecHdrTab[pseg->SDshtidx].addr,
            SecHdrTab[SegData[seg]->SDshtidx].addr,
            *p,
            SecHdrTab[SegData[seg]->SDshtidx].addr -
            (SecHdrTab[pseg->SDshtidx].addr + offset));
#endif
        *p += SecHdrTab[SegData[seg]->SDshtidx].addr -
              (SecHdrTab[pseg->SDshtidx].addr - value);
    }
}

/***************************
 * Number symbols so they are
 * ordered as locals, public and then extern/comdef
 */

void mach_numbersyms()
{
    //printf("mach_numbersyms()\n");
    int n = 0;

    int dim;
    dim = local_symbuf->size() / sizeof(symbol *);
    for (int i = 0; i < dim; i++)
    {   symbol *s = ((symbol **)local_symbuf->buf)[i];
        s->Sxtrnnum = n;
        n++;
    }

    dim = public_symbuf->size() / sizeof(symbol *);
    for (int i = 0; i < dim; i++)
    {   symbol *s = ((symbol **)public_symbuf->buf)[i];
        s->Sxtrnnum = n;
        n++;
    }

    dim = extern_symbuf->size() / sizeof(symbol *);
    for (int i = 0; i < dim; i++)
    {   symbol *s = ((symbol **)extern_symbuf->buf)[i];
        s->Sxtrnnum = n;
        n++;
    }

    dim = comdef_symbuf->size() / sizeof(Comdef);
    for (int i = 0; i < dim; i++)
    {   Comdef *c = ((Comdef *)comdef_symbuf->buf) + i;
        c->sym->Sxtrnnum = n;
        n++;
    }
}


/***************************
 * Fixup and terminate object file.
 */

void Obj::termfile()
{
    //dbg_printf("Obj::termfile\n");
    if (configv.addlinenumbers)
    {
        dwarf_termmodule();
    }
}

/*********************************
 * Terminate package.
 */

void Obj::term(const char *objfilename)
{
    //printf("Obj::term()\n");
#if SCPP
    if (!errcnt)
#endif
    {
        outfixlist();           // backpatches
    }

    if (configv.addlinenumbers)
    {
        dwarf_termfile();
    }

#if SCPP
    if (errcnt)
        return;
#endif

    /* Write out the object file in the following order:
     *  header
     *  commands
     *          segment_command
     *                  { sections }
     *          symtab_command
     *          dysymtab_command
     *  { segment contents }
     *  { relocations }
     *  symbol table
     *  string table
     *  indirect symbol table
     */

    unsigned foffset;
    unsigned headersize;
    unsigned sizeofcmds;

    // Write out the bytes for the header
    if (I64)
    {
        mach_header_64 header;

        header.magic = MH_MAGIC_64;
        header.cputype = CPU_TYPE_X86_64;
        header.cpusubtype = CPU_SUBTYPE_I386_ALL;
        header.filetype = MH_OBJECT;
        header.ncmds = 3;
        header.sizeofcmds = sizeof(segment_command_64) +
                                (section_cnt - 1) * sizeof(struct section_64) +
                            sizeof(symtab_command) +
                            sizeof(dysymtab_command);
        header.flags = MH_SUBSECTIONS_VIA_SYMBOLS;
        header.reserved = 0;
        fobjbuf->write(&header, sizeof(header));
        foffset = sizeof(header);       // start after header
        headersize = sizeof(header);
        sizeofcmds = header.sizeofcmds;

        // Write the actual data later
        fobjbuf->writezeros(header.sizeofcmds);
        foffset += header.sizeofcmds;
    }
    else
    {
        mach_header header;

        header.magic = MH_MAGIC;
        header.cputype = CPU_TYPE_I386;
        header.cpusubtype = CPU_SUBTYPE_I386_ALL;
        header.filetype = MH_OBJECT;
        header.ncmds = 3;
        header.sizeofcmds = sizeof(segment_command) +
                                (section_cnt - 1) * sizeof(struct section) +
                            sizeof(symtab_command) +
                            sizeof(dysymtab_command);
        header.flags = MH_SUBSECTIONS_VIA_SYMBOLS;
        fobjbuf->write(&header, sizeof(header));
        foffset = sizeof(header);       // start after header
        headersize = sizeof(header);
        sizeofcmds = header.sizeofcmds;

        // Write the actual data later
        fobjbuf->writezeros(header.sizeofcmds);
        foffset += header.sizeofcmds;
    }

    struct segment_command segment_cmd;
    struct segment_command_64 segment_cmd64;
    struct symtab_command symtab_cmd;
    struct dysymtab_command dysymtab_cmd;

    memset(&segment_cmd, 0, sizeof(segment_cmd));
    memset(&segment_cmd64, 0, sizeof(segment_cmd64));
    memset(&symtab_cmd, 0, sizeof(symtab_cmd));
    memset(&dysymtab_cmd, 0, sizeof(dysymtab_cmd));

    if (I64)
    {
        segment_cmd64.cmd = LC_SEGMENT_64;
        segment_cmd64.cmdsize = sizeof(segment_cmd64) +
                                    (section_cnt - 1) * sizeof(struct section_64);
        segment_cmd64.nsects = section_cnt - 1;
        segment_cmd64.maxprot = 7;
        segment_cmd64.initprot = 7;
    }
    else
    {
        segment_cmd.cmd = LC_SEGMENT;
        segment_cmd.cmdsize = sizeof(segment_cmd) +
                                    (section_cnt - 1) * sizeof(struct section);
        segment_cmd.nsects = section_cnt - 1;
        segment_cmd.maxprot = 7;
        segment_cmd.initprot = 7;
    }

    symtab_cmd.cmd = LC_SYMTAB;
    symtab_cmd.cmdsize = sizeof(symtab_cmd);

    dysymtab_cmd.cmd = LC_DYSYMTAB;
    dysymtab_cmd.cmdsize = sizeof(dysymtab_cmd);

    /* If a __pointers section was emitted, need to set the .reserved1
     * field to the symbol index in the indirect symbol table of the
     * start of the __pointers symbols.
     */
    if (pointersSeg)
    {
        seg_data *pseg = SegData[pointersSeg];
        if (I64)
        {
            struct section_64 *psechdr = &SecHdrTab64[pseg->SDshtidx]; // corresponding section
            psechdr->reserved1 = indirectsymbuf1
                ? indirectsymbuf1->size() / sizeof(Symbol *)
                : 0;
        }
        else
        {
            struct section *psechdr = &SecHdrTab[pseg->SDshtidx]; // corresponding section
            psechdr->reserved1 = indirectsymbuf1
                ? indirectsymbuf1->size() / sizeof(Symbol *)
                : 0;
        }
    }

    // Walk through sections determining size and file offsets

    //
    // First output individual section data associate with program
    //  code and data
    //
    foffset = elf_align(I64 ? 8 : 4, foffset);
    if (I64)
        segment_cmd64.fileoff = foffset;
    else
        segment_cmd.fileoff = foffset;
    unsigned vmaddr = 0;

    //printf("Setup offsets and sizes foffset %d\n\tsection_cnt %d, seg_count %d\n",foffset,section_cnt,seg_count);
    // Zero filled segments go at the end, so go through segments twice
    for (int i = 0; i < 2; i++)
    {
        for (int seg = 1; seg <= seg_count; seg++)
        {
            seg_data *pseg = SegData[seg];
            if (I64)
            {
                struct section_64 *psechdr = &SecHdrTab64[pseg->SDshtidx]; // corresponding section

                // Do zero-fill the second time through this loop
                if (i ^ (psechdr->flags == S_ZEROFILL))
                    continue;

                int align = 1 << psechdr->align;
                while (psechdr->align > 0 && align < pseg->SDalignment)
                {
                    psechdr->align += 1;
                    align <<= 1;
                }
                foffset = elf_align(align, foffset);
                vmaddr = (vmaddr + align - 1) & ~(align - 1);
                if (psechdr->flags == S_ZEROFILL)
                {
                    psechdr->offset = 0;
                    psechdr->size = pseg->SDoffset; // accumulated size
                }
                else
                {
                    psechdr->offset = foffset;
                    psechdr->size = 0;
                    //printf("\tsection name %s,", psechdr->sectname);
                    if (pseg->SDbuf && pseg->SDbuf->size())
                    {
                        //printf("\tsize %d\n", pseg->SDbuf->size());
                        psechdr->size = pseg->SDbuf->size();
                        fobjbuf->write(pseg->SDbuf->buf, psechdr->size);
                        foffset += psechdr->size;
                    }
                }
                psechdr->addr = vmaddr;
                vmaddr += psechdr->size;
                //printf(" assigned offset %d, size %d\n", foffset, psechdr->sh_size);
            }
            else
            {
                struct section *psechdr = &SecHdrTab[pseg->SDshtidx]; // corresponding section

                // Do zero-fill the second time through this loop
                if (i ^ (psechdr->flags == S_ZEROFILL))
                    continue;

                int align = 1 << psechdr->align;
                while (psechdr->align > 0 && align < pseg->SDalignment)
                {
                    psechdr->align += 1;
                    align <<= 1;
                }
                foffset = elf_align(align, foffset);
                vmaddr = (vmaddr + align - 1) & ~(align - 1);
                if (psechdr->flags == S_ZEROFILL)
                {
                    psechdr->offset = 0;
                    psechdr->size = pseg->SDoffset; // accumulated size
                }
                else
                {
                    psechdr->offset = foffset;
                    psechdr->size = 0;
                    //printf("\tsection name %s,", psechdr->sectname);
                    if (pseg->SDbuf && pseg->SDbuf->size())
                    {
                        //printf("\tsize %d\n", pseg->SDbuf->size());
                        psechdr->size = pseg->SDbuf->size();
                        fobjbuf->write(pseg->SDbuf->buf, psechdr->size);
                        foffset += psechdr->size;
                    }
                }
                psechdr->addr = vmaddr;
                vmaddr += psechdr->size;
                //printf(" assigned offset %d, size %d\n", foffset, psechdr->sh_size);
            }
        }
    }

    if (I64)
    {
        segment_cmd64.vmsize = vmaddr;
        segment_cmd64.filesize = foffset - segment_cmd64.fileoff;
        /* Bugzilla 5331: Apparently having the filesize field greater than the vmsize field is an
         * error, and is happening sometimes.
         */
        if (segment_cmd64.filesize > vmaddr)
            segment_cmd64.vmsize = segment_cmd64.filesize;
    }
    else
    {
        segment_cmd.vmsize = vmaddr;
        segment_cmd.filesize = foffset - segment_cmd.fileoff;
        /* Bugzilla 5331: Apparently having the filesize field greater than the vmsize field is an
         * error, and is happening sometimes.
         */
        if (segment_cmd.filesize > vmaddr)
            segment_cmd.vmsize = segment_cmd.filesize;
    }

    // Put out relocation data
    mach_numbersyms();
    for (int seg = 1; seg <= seg_count; seg++)
    {
        seg_data *pseg = SegData[seg];
        struct section *psechdr = NULL;
        struct section_64 *psechdr64 = NULL;
        if (I64)
        {
            psechdr64 = &SecHdrTab64[pseg->SDshtidx];   // corresponding section
            //printf("psechdr->addr = x%llx\n", psechdr64->addr);
        }
        else
        {
            psechdr = &SecHdrTab[pseg->SDshtidx];   // corresponding section
            //printf("psechdr->addr = x%x\n", psechdr->addr);
        }
        foffset = elf_align(I64 ? 8 : 4, foffset);
        unsigned reloff = foffset;
        unsigned nreloc = 0;
        if (pseg->SDrel)
        {   Relocation *r = (Relocation *)pseg->SDrel->buf;
            Relocation *rend = (Relocation *)(pseg->SDrel->buf + pseg->SDrel->size());
            for (; r != rend; r++)
            {   symbol *s = r->targsym;
                const char *rs = r->rtype == RELaddr ? "addr" : "rel";
                //printf("%d:x%04llx : tseg %d tsym %s REL%s\n", seg, r->offset, r->targseg, s ? s->Sident : "0", rs);
                relocation_info rel;
                scattered_relocation_info srel;
                if (s)
                {
                    //printf("Relocation\n");
                    //symbol_print(s);
                    if (r->flag == 1)
                    {
                        if (I64)
                        {
                            rel.r_type = X86_64_RELOC_SUBTRACTOR;
                            rel.r_address = r->offset;
                            rel.r_symbolnum = r->funcsym->Sxtrnnum;
                            rel.r_pcrel = 0;
                            rel.r_length = 3;
                            rel.r_extern = 1;
                            fobjbuf->write(&rel, sizeof(rel));
                            foffset += sizeof(rel);
                            ++nreloc;

                            rel.r_type = X86_64_RELOC_UNSIGNED;
                            rel.r_symbolnum = s->Sxtrnnum;
                            fobjbuf->write(&rel, sizeof(rel));
                            foffset += sizeof(rel);
                            ++nreloc;

                            // patch with fdesym->Soffset - offset
                            int64_t *p = (int64_t *)patchAddr64(seg, r->offset);
                            *p += r->funcsym->Soffset - r->offset;
                            continue;
                        }
                        else
                        {
                            // address = segment + offset
                            int targ_address = SecHdrTab[SegData[s->Sseg]->SDshtidx].addr + s->Soffset;
                            int fixup_address = psechdr->addr + r->offset;

                            srel.r_scattered = 1;
                            srel.r_type = GENERIC_RELOC_LOCAL_SECTDIFF;
                            srel.r_address = r->offset;
                            srel.r_pcrel = 0;
                            srel.r_length = 2;
                            srel.r_value = targ_address;
                            fobjbuf->write(&srel, sizeof(srel));
                            foffset += sizeof(srel);
                            ++nreloc;

                            srel.r_type = GENERIC_RELOC_PAIR;
                            srel.r_address = 0;
                            srel.r_value = fixup_address;
                            fobjbuf->write(&srel, sizeof(srel));
                            foffset += sizeof(srel);
                            ++nreloc;

                            int32_t *p = patchAddr(seg, r->offset);
                            *p += targ_address - fixup_address;
                            continue;
                        }
                    }
                    else if (pseg->isCode())
                    {
                        if (I64)
                        {
                            rel.r_type = (r->rtype == RELrel)
                                    ? X86_64_RELOC_BRANCH
                                    : X86_64_RELOC_SIGNED;
                            if (r->val == -1)
                                rel.r_type = X86_64_RELOC_SIGNED_1;
                            else if (r->val == -2)
                                rel.r_type = X86_64_RELOC_SIGNED_2;
                            if (r->val == -4)
                                rel.r_type = X86_64_RELOC_SIGNED_4;

                            if (s->Sclass == SCextern ||
                                s->Sclass == SCcomdef ||
                                s->Sclass == SCcomdat ||
                                s->Sclass == SCglobal)
                            {
                                if (I64 && (s->ty() & mTYLINK) == mTYthread && r->rtype == RELaddr)
                                    rel.r_type = X86_64_RELOC_TLV;
                                else if ((s->Sfl == FLfunc || s->Sfl == FLextern || s->Sclass == SCglobal || s->Sclass == SCcomdat || s->Sclass == SCcomdef) && r->rtype == RELaddr)
                                {
                                    rel.r_type = X86_64_RELOC_GOT_LOAD;
                                    if (seg == eh_frame_seg ||
                                        seg == except_table_seg)
                                        rel.r_type = X86_64_RELOC_GOT;
                                }
                                rel.r_address = r->offset;
                                rel.r_symbolnum = s->Sxtrnnum;
                                rel.r_pcrel = 1;
                                rel.r_length = 2;
                                rel.r_extern = 1;
                                fobjbuf->write(&rel, sizeof(rel));
                                foffset += sizeof(rel);
                                nreloc++;
                                continue;
                            }
                            else
                            {
                                rel.r_address = r->offset;
                                rel.r_symbolnum = s->Sseg;
                                rel.r_pcrel = 1;
                                rel.r_length = 2;
                                rel.r_extern = 0;
                                fobjbuf->write(&rel, sizeof(rel));
                                foffset += sizeof(rel);
                                nreloc++;

                                int32_t *p = patchAddr64(seg, r->offset);
                                // Absolute address; add in addr of start of targ seg
//printf("*p = x%x, .addr = x%x, Soffset = x%x\n", *p, (int)SecHdrTab64[SegData[s->Sseg]->SDshtidx].addr, (int)s->Soffset);
//printf("pseg = x%x, r->offset = x%x\n", (int)SecHdrTab64[pseg->SDshtidx].addr, (int)r->offset);
                                *p += SecHdrTab64[SegData[s->Sseg]->SDshtidx].addr;
                                *p += s->Soffset;
                                *p -= SecHdrTab64[pseg->SDshtidx].addr + r->offset + 4;
                                //patch(pseg, r->offset, s->Sseg, s->Soffset);
                                continue;
                            }
                        }
                    }
                    else
                    {
                        if (s->Sclass == SCextern ||
                            s->Sclass == SCcomdef ||
                            s->Sclass == SCcomdat)
                        {
                            rel.r_address = r->offset;
                            rel.r_symbolnum = s->Sxtrnnum;
                            rel.r_pcrel = 0;
                            rel.r_length = 2;
                            rel.r_extern = 1;
                            rel.r_type = GENERIC_RELOC_VANILLA;
                            if (I64)
                            {
                                rel.r_type = X86_64_RELOC_UNSIGNED;
                                rel.r_length = 3;
                            }
                            fobjbuf->write(&rel, sizeof(rel));
                            foffset += sizeof(rel);
                            nreloc++;
                            continue;
                        }
                        else
                        {
                            rel.r_address = r->offset;
                            rel.r_symbolnum = s->Sseg;
                            rel.r_pcrel = 0;
                            rel.r_length = 2;
                            rel.r_extern = 0;
                            rel.r_type = GENERIC_RELOC_VANILLA;
                            if (I64)
                            {
                                rel.r_type = X86_64_RELOC_UNSIGNED;
                                rel.r_length = 3;
                                if (0 && s->Sseg != seg)
                                    rel.r_type = X86_64_RELOC_BRANCH;
                            }
                            fobjbuf->write(&rel, sizeof(rel));
                            foffset += sizeof(rel);
                            nreloc++;
                            if (I64)
                            {
                                rel.r_length = 3;
                                int32_t *p = patchAddr64(seg, r->offset);
                                // Absolute address; add in addr of start of targ seg
                                *p += SecHdrTab64[SegData[s->Sseg]->SDshtidx].addr + s->Soffset;
                                //patch(pseg, r->offset, s->Sseg, s->Soffset);
                            }
                            else
                            {
                                int32_t *p = patchAddr(seg, r->offset);
                                // Absolute address; add in addr of start of targ seg
                                *p += SecHdrTab[SegData[s->Sseg]->SDshtidx].addr + s->Soffset;
                                //patch(pseg, r->offset, s->Sseg, s->Soffset);
                            }
                            continue;
                        }
                    }
                }
                else if (r->rtype == RELaddr && pseg->isCode())
                {
                    srel.r_scattered = 1;

                    srel.r_address = r->offset;
                    srel.r_length = 2;
                    if (I64)
                    {
                        int32_t *p64 = patchAddr64(seg, r->offset);
                        srel.r_type = X86_64_RELOC_GOT;
                        srel.r_value = SecHdrTab64[SegData[r->targseg]->SDshtidx].addr + *p64;
                        //printf("SECTDIFF: x%llx + x%llx = x%x\n", SecHdrTab[SegData[r->targseg]->SDshtidx].addr, *p, srel.r_value);
                    }
                    else
                    {
                        int32_t *p = patchAddr(seg, r->offset);
                        srel.r_type = GENERIC_RELOC_LOCAL_SECTDIFF;
                        srel.r_value = SecHdrTab[SegData[r->targseg]->SDshtidx].addr + *p;
                        //printf("SECTDIFF: x%x + x%x = x%x\n", SecHdrTab[SegData[r->targseg]->SDshtidx].addr, *p, srel.r_value);
                    }
                    srel.r_pcrel = 0;
                    fobjbuf->write(&srel, sizeof(srel));
                    foffset += sizeof(srel);
                    nreloc++;

                    srel.r_address = 0;
                    srel.r_length = 2;
                    if (I64)
                    {
                        srel.r_type = X86_64_RELOC_SIGNED;
                        srel.r_value = SecHdrTab64[pseg->SDshtidx].addr +
                                r->funcsym->Slocalgotoffset + NPTRSIZE;
                    }
                    else
                    {
                        srel.r_type = GENERIC_RELOC_PAIR;
                        if (r->funcsym)
                            srel.r_value = SecHdrTab[pseg->SDshtidx].addr +
                                    r->funcsym->Slocalgotoffset + NPTRSIZE;
                        else
                            srel.r_value = psechdr->addr + r->offset;
                        //printf("srel.r_value = x%x, psechdr->addr = x%x, r->offset = x%x\n",
                            //(int)srel.r_value, (int)psechdr->addr, (int)r->offset);
                    }
                    srel.r_pcrel = 0;
                    fobjbuf->write(&srel, sizeof(srel));
                    foffset += sizeof(srel);
                    nreloc++;

                    // Recalc due to possible realloc of fobjbuf->buf
                    if (I64)
                    {
                        int32_t *p64 = patchAddr64(seg, r->offset);
                        //printf("address = x%x, p64 = %p *p64 = x%llx\n", r->offset, p64, *p64);
                        *p64 += SecHdrTab64[SegData[r->targseg]->SDshtidx].addr -
                              (SecHdrTab64[pseg->SDshtidx].addr + r->funcsym->Slocalgotoffset + NPTRSIZE);
                    }
                    else
                    {
                        int32_t *p = patchAddr(seg, r->offset);
                        //printf("address = x%x, p = %p *p = x%x\n", r->offset, p, *p);
                        if (r->funcsym)
                            *p += SecHdrTab[SegData[r->targseg]->SDshtidx].addr -
                                  (SecHdrTab[pseg->SDshtidx].addr + r->funcsym->Slocalgotoffset + NPTRSIZE);
                        else
                            // targ_address - fixup_address
                            *p += SecHdrTab[SegData[r->targseg]->SDshtidx].addr -
                                  (psechdr->addr + r->offset);
                    }
                    continue;
                }
                else
                {
                    rel.r_address = r->offset;
                    rel.r_symbolnum = r->targseg;
                    rel.r_pcrel = (r->rtype == RELaddr) ? 0 : 1;
                    rel.r_length = 2;
                    rel.r_extern = 0;
                    rel.r_type = GENERIC_RELOC_VANILLA;
                    if (I64)
                    {
                        rel.r_type = X86_64_RELOC_UNSIGNED;
                        rel.r_length = 3;
                        if (0 && r->targseg != seg)
                            rel.r_type = X86_64_RELOC_BRANCH;
                    }
                    fobjbuf->write(&rel, sizeof(rel));
                    foffset += sizeof(rel);
                    nreloc++;
                    if (I64)
                    {
                        int32_t *p64 = patchAddr64(seg, r->offset);
                        //int64_t before = *p64;
                        if (rel.r_pcrel)
                            // Relative address
                            patch(pseg, r->offset, r->targseg, 0);
                        else
                        {   // Absolute address; add in addr of start of targ seg
//printf("*p = x%x, targ.addr = x%x\n", *p64, (int)SecHdrTab64[SegData[r->targseg]->SDshtidx].addr);
//printf("pseg = x%x, r->offset = x%x\n", (int)SecHdrTab64[pseg->SDshtidx].addr, (int)r->offset);
                            *p64 += SecHdrTab64[SegData[r->targseg]->SDshtidx].addr;
                            //*p64 -= SecHdrTab64[pseg->SDshtidx].addr;
                        }
                        //printf("%d:x%04x before = x%04llx, after = x%04llx pcrel = %d\n", seg, r->offset, before, *p64, rel.r_pcrel);
                    }
                    else
                    {
                        int32_t *p = patchAddr(seg, r->offset);
                        //int32_t before = *p;
                        if (rel.r_pcrel)
                            // Relative address
                            patch(pseg, r->offset, r->targseg, 0);
                        else
                            // Absolute address; add in addr of start of targ seg
                            *p += SecHdrTab[SegData[r->targseg]->SDshtidx].addr;
                        //printf("%d:x%04x before = x%04x, after = x%04x pcrel = %d\n", seg, r->offset, before, *p, rel.r_pcrel);
                    }
                    continue;
                }
            }
        }
        if (nreloc)
        {
            if (I64)
            {
                psechdr64->reloff = reloff;
                psechdr64->nreloc = nreloc;
            }
            else
            {
                psechdr->reloff = reloff;
                psechdr->nreloc = nreloc;
            }
        }
    }

    // Put out symbol table
    foffset = elf_align(I64 ? 8 : 4, foffset);
    symtab_cmd.symoff = foffset;
    dysymtab_cmd.ilocalsym = 0;
    dysymtab_cmd.nlocalsym  = local_symbuf->size() / sizeof(symbol *);
    dysymtab_cmd.iextdefsym = dysymtab_cmd.nlocalsym;
    dysymtab_cmd.nextdefsym = public_symbuf->size() / sizeof(symbol *);
    dysymtab_cmd.iundefsym = dysymtab_cmd.iextdefsym + dysymtab_cmd.nextdefsym;
    int nexterns = extern_symbuf->size() / sizeof(symbol *);
    int ncomdefs = comdef_symbuf->size() / sizeof(Comdef);
    dysymtab_cmd.nundefsym  = nexterns + ncomdefs;
    symtab_cmd.nsyms =  dysymtab_cmd.nlocalsym +
                        dysymtab_cmd.nextdefsym +
                        dysymtab_cmd.nundefsym;
    fobjbuf->reserve(symtab_cmd.nsyms * (I64 ? sizeof(struct nlist_64) : sizeof(struct nlist)));
    for (int i = 0; i < dysymtab_cmd.nlocalsym; i++)
    {   symbol *s = ((symbol **)local_symbuf->buf)[i];
        struct nlist_64 sym;
        sym.n_un.n_strx = elf_addmangled(s);
        sym.n_type = N_SECT;
        sym.n_desc = 0;
        if (s->Sclass == SCcomdat)
            sym.n_desc = N_WEAK_DEF;
        sym.n_sect = s->Sseg;
        if (I64)
        {
            sym.n_value = s->Soffset + SecHdrTab64[SegData[s->Sseg]->SDshtidx].addr;
            fobjbuf->write(&sym, sizeof(sym));
        }
        else
        {
            struct nlist sym32;
            sym32.n_un.n_strx = sym.n_un.n_strx;
            sym32.n_value = s->Soffset + SecHdrTab[SegData[s->Sseg]->SDshtidx].addr;
            sym32.n_type = sym.n_type;
            sym32.n_desc = sym.n_desc;
            sym32.n_sect = sym.n_sect;
            fobjbuf->write(&sym32, sizeof(sym32));
        }
    }
    for (int i = 0; i < dysymtab_cmd.nextdefsym; i++)
    {   symbol *s = ((symbol **)public_symbuf->buf)[i];

        //printf("Writing public symbol %d:x%x %s\n", s->Sseg, s->Soffset, s->Sident);
        struct nlist_64 sym;
        sym.n_un.n_strx = elf_addmangled(s);
        sym.n_type = N_EXT | N_SECT;
        sym.n_desc = 0;
        if (s->Sclass == SCcomdat)
            sym.n_desc = N_WEAK_DEF;
        sym.n_sect = s->Sseg;
        if (I64)
        {
            sym.n_value = s->Soffset + SecHdrTab64[SegData[s->Sseg]->SDshtidx].addr;
            fobjbuf->write(&sym, sizeof(sym));
        }
        else
        {
            struct nlist sym32;
            sym32.n_un.n_strx = sym.n_un.n_strx;
            sym32.n_value = s->Soffset + SecHdrTab[SegData[s->Sseg]->SDshtidx].addr;
            sym32.n_type = sym.n_type;
            sym32.n_desc = sym.n_desc;
            sym32.n_sect = sym.n_sect;
            fobjbuf->write(&sym32, sizeof(sym32));
        }
    }
    for (int i = 0; i < nexterns; i++)
    {   symbol *s = ((symbol **)extern_symbuf->buf)[i];
        struct nlist_64 sym;
        sym.n_un.n_strx = elf_addmangled(s);
        sym.n_value = s->Soffset;
        sym.n_type = N_EXT | N_UNDF;
        sym.n_desc = tyfunc(s->ty()) ? REFERENCE_FLAG_UNDEFINED_LAZY
                                     : REFERENCE_FLAG_UNDEFINED_NON_LAZY;
        sym.n_sect = 0;
        if (I64)
            fobjbuf->write(&sym, sizeof(sym));
        else
        {
            struct nlist sym32;
            sym32.n_un.n_strx = sym.n_un.n_strx;
            sym32.n_value = sym.n_value;
            sym32.n_type = sym.n_type;
            sym32.n_desc = sym.n_desc;
            sym32.n_sect = sym.n_sect;
            fobjbuf->write(&sym32, sizeof(sym32));
        }
    }
    for (int i = 0; i < ncomdefs; i++)
    {   Comdef *c = ((Comdef *)comdef_symbuf->buf) + i;
        struct nlist_64 sym;
        sym.n_un.n_strx = elf_addmangled(c->sym);
        sym.n_value = c->size * c->count;
        sym.n_type = N_EXT | N_UNDF;
        int align;
        if (c->size < 2)
            align = 0;          // align is expressed as power of 2
        else if (c->size < 4)
            align = 1;
        else if (c->size < 8)
            align = 2;
        else if (c->size < 16)
            align = 3;
        else
            align = 4;
        sym.n_desc = align << 8;
        sym.n_sect = 0;
        if (I64)
            fobjbuf->write(&sym, sizeof(sym));
        else
        {
            struct nlist sym32;
            sym32.n_un.n_strx = sym.n_un.n_strx;
            sym32.n_value = sym.n_value;
            sym32.n_type = sym.n_type;
            sym32.n_desc = sym.n_desc;
            sym32.n_sect = sym.n_sect;
            fobjbuf->write(&sym32, sizeof(sym32));
        }
    }
    if (extdef)
    {
        struct nlist_64 sym;
        sym.n_un.n_strx = extdef;
        sym.n_value = 0;
        sym.n_type = N_EXT | N_UNDF;
        sym.n_desc = 0;
        sym.n_sect = 0;
        if (I64)
            fobjbuf->write(&sym, sizeof(sym));
        else
        {
            struct nlist sym32;
            sym32.n_un.n_strx = sym.n_un.n_strx;
            sym32.n_value = sym.n_value;
            sym32.n_type = sym.n_type;
            sym32.n_desc = sym.n_desc;
            sym32.n_sect = sym.n_sect;
            fobjbuf->write(&sym32, sizeof(sym32));
        }
        symtab_cmd.nsyms++;
    }
    foffset += symtab_cmd.nsyms * (I64 ? sizeof(struct nlist_64) : sizeof(struct nlist));

    // Put out string table
    foffset = elf_align(I64 ? 8 : 4, foffset);
    symtab_cmd.stroff = foffset;
    symtab_cmd.strsize = symtab_strings->size();
    fobjbuf->write(symtab_strings->buf, symtab_cmd.strsize);
    foffset += symtab_cmd.strsize;

    // Put out indirectsym table, which is in two parts
    foffset = elf_align(I64 ? 8 : 4, foffset);
    dysymtab_cmd.indirectsymoff = foffset;
    if (indirectsymbuf1)
    {   dysymtab_cmd.nindirectsyms += indirectsymbuf1->size() / sizeof(Symbol *);
        for (int i = 0; i < dysymtab_cmd.nindirectsyms; i++)
        {   Symbol *s = ((Symbol **)indirectsymbuf1->buf)[i];
            fobjbuf->write32(s->Sxtrnnum);
        }
    }
    if (indirectsymbuf2)
    {   int n = indirectsymbuf2->size() / sizeof(Symbol *);
        dysymtab_cmd.nindirectsyms += n;
        for (int i = 0; i < n; i++)
        {   Symbol *s = ((Symbol **)indirectsymbuf2->buf)[i];
            fobjbuf->write32(s->Sxtrnnum);
        }
    }
    foffset += dysymtab_cmd.nindirectsyms * 4;

    /* The correct offsets are now determined, so
     * rewind and fix the header.
     */
    fobjbuf->position(headersize, sizeofcmds);
    if (I64)
    {
        fobjbuf->write(&segment_cmd64, sizeof(segment_cmd64));
        fobjbuf->write(SECbuf->buf + sizeof(struct section_64), (section_cnt - 1) * sizeof(struct section_64));
    }
    else
    {
        fobjbuf->write(&segment_cmd, sizeof(segment_cmd));
        fobjbuf->write(SECbuf->buf + sizeof(struct section), (section_cnt - 1) * sizeof(struct section));
    }
    fobjbuf->write(&symtab_cmd, sizeof(symtab_cmd));
    fobjbuf->write(&dysymtab_cmd, sizeof(dysymtab_cmd));
    fobjbuf->position(foffset, 0);
    fobjbuf->flush();
}

/*****************************
 * Line number support.
 */

/***************************
 * Record file and line number at segment and offset.
 * The actual .debug_line segment is put out by dwarf_termfile().
 * Params:
 *      srcpos = source file position
 *      seg = segment it corresponds to
 *      offset = offset within seg
 */

void Obj::linnum(Srcpos srcpos, int seg, targ_size_t offset)
{
    if (srcpos.Slinnum == 0)
        return;

#if 0
#if MARS || SCPP
    printf("Obj::linnum(seg=%d, offset=x%lx) ", seg, offset);
#endif
    srcpos.print("");
#endif

#if MARS
    if (!srcpos.Sfilename)
        return;
#endif
#if SCPP
    if (!srcpos.Sfilptr)
        return;
    sfile_debug(&srcpos_sfile(srcpos));
    Sfile *sf = *srcpos.Sfilptr;
#endif

    size_t i;
    seg_data *pseg = SegData[seg];

    // Find entry i in SDlinnum_data[] that corresponds to srcpos filename
    for (i = 0; 1; i++)
    {
        if (i == pseg->SDlinnum_count)
        {   // Create new entry
            if (pseg->SDlinnum_count == pseg->SDlinnum_max)
            {   // Enlarge array
                unsigned newmax = pseg->SDlinnum_max * 2 + 1;
                //printf("realloc %d\n", newmax * sizeof(linnum_data));
                pseg->SDlinnum_data = (linnum_data *)mem_realloc(
                    pseg->SDlinnum_data, newmax * sizeof(linnum_data));
                memset(pseg->SDlinnum_data + pseg->SDlinnum_max, 0,
                    (newmax - pseg->SDlinnum_max) * sizeof(linnum_data));
                pseg->SDlinnum_max = newmax;
            }
            pseg->SDlinnum_count++;
#if MARS
            pseg->SDlinnum_data[i].filename = srcpos.Sfilename;
#endif
#if SCPP
            pseg->SDlinnum_data[i].filptr = sf;
#endif
            break;
        }
#if MARS
        if (pseg->SDlinnum_data[i].filename == srcpos.Sfilename)
#endif
#if SCPP
        if (pseg->SDlinnum_data[i].filptr == sf)
#endif
            break;
    }

    linnum_data *ld = &pseg->SDlinnum_data[i];
//    printf("i = %d, ld = x%x\n", i, ld);
    if (ld->linoff_count == ld->linoff_max)
    {
        if (!ld->linoff_max)
            ld->linoff_max = 8;
        ld->linoff_max *= 2;
        ld->linoff = (unsigned (*)[2])mem_realloc(ld->linoff, ld->linoff_max * sizeof(unsigned) * 2);
    }
    ld->linoff[ld->linoff_count][0] = srcpos.Slinnum;
    ld->linoff[ld->linoff_count][1] = offset;
    ld->linoff_count++;
}


/*******************************
 * Set start address
 */

void Obj::startaddress(Symbol *s)
{
    //dbg_printf("Obj::startaddress(Symbol *%s)\n",s->Sident);
    //obj.startaddress = s;
}

/*******************************
 * Output library name.
 */

bool Obj::includelib(const char *name)
{
    //dbg_printf("Obj::includelib(name *%s)\n",name);
    return false;
}

/**********************************
 * Do we allow zero sized objects?
 */

bool Obj::allowZeroSize()
{
    return true;
}

/**************************
 * Embed string in executable.
 */

void Obj::exestr(const char *p)
{
    //dbg_printf("Obj::exestr(char *%s)\n",p);
}

/**************************
 * Embed string in obj.
 */

void Obj::user(const char *p)
{
    //dbg_printf("Obj::user(char *%s)\n",p);
}

/*******************************
 * Output a weak extern record.
 */

void Obj::wkext(Symbol *s1,Symbol *s2)
{
    //dbg_printf("Obj::wkext(Symbol *%s,Symbol *s2)\n",s1->Sident,s2->Sident);
}

/*******************************
 * Output file name record.
 *
 * Currently assumes that obj_filename will not be called
 *      twice for the same file.
 */

void obj_filename(const char *modname)
{
    //dbg_printf("obj_filename(char *%s)\n",modname);
    // Not supported by Mach-O
}

/*******************************
 * Embed compiler version in .obj file.
 */

void Obj::compiler()
{
    //dbg_printf("Obj::compiler\n");
}


/**************************************
 * Symbol is the function that calls the static constructors.
 * Put a pointer to it into a special segment that the startup code
 * looks at.
 * Input:
 *      s       static constructor function
 *      dtor    !=0 if leave space for static destructor
 *      seg     1:      user
 *              2:      lib
 *              3:      compiler
 */

void Obj::staticctor(Symbol *s, int, int)
{
    setModuleCtorDtor(s, true);
}

/**************************************
 * Symbol is the function that calls the static destructors.
 * Put a pointer to it into a special segment that the exit code
 * looks at.
 * Input:
 *      s       static destructor function
 */

void Obj::staticdtor(Symbol *s)
{
    setModuleCtorDtor(s, false);
}


/***************************************
 * Stuff pointer to function in its own segment.
 * Used for static ctor and dtor lists.
 */

void Obj::setModuleCtorDtor(Symbol *sfunc, bool isCtor)
{
    const char *secname = isCtor ? "__mod_init_func" : "__mod_term_func";
    const int align = I64 ? 3 : 2; // align to NPTRSIZE
    const int flags = isCtor ? S_MOD_INIT_FUNC_POINTERS : S_MOD_TERM_FUNC_POINTERS;
    IDXSEC seg = MachObj::getsegment(secname, "__DATA", align, flags);

    const int relflags = I64 ? CFoff | CFoffset64 : CFoff;
    const int sz = Obj::reftoident(seg, SegData[seg]->SDoffset, sfunc, 0, relflags);
    SegData[seg]->SDoffset += sz;
}


/***************************************
 * Stuff the following data (instance of struct FuncTable) in a separate segment:
 *      pointer to function
 *      pointer to ehsym
 *      length of function
 */

void Obj::ehtables(Symbol *sfunc,unsigned size,Symbol *ehsym)
{
    //dbg_printf("Obj::ehtables(%s) \n",sfunc->Sident);

    /* BUG: this should go into a COMDAT if sfunc is in a COMDAT
     * otherwise the duplicates aren't removed.
     */

    int align = I64 ? 3 : 2;            // align to NPTRSIZE
    // The size is sizeof(struct FuncTable) in deh2.d
    int seg = MachObj::getsegment("__deh_eh", "__DATA", align, S_REGULAR);

    Outbuffer *buf = SegData[seg]->SDbuf;
    if (I64)
    {   Obj::reftoident(seg, buf->size(), sfunc, 0, CFoff | CFoffset64);
        Obj::reftoident(seg, buf->size(), ehsym, 0, CFoff | CFoffset64);
        buf->write64(sfunc->Ssize);
    }
    else
    {   Obj::reftoident(seg, buf->size(), sfunc, 0, CFoff);
        Obj::reftoident(seg, buf->size(), ehsym, 0, CFoff);
        buf->write32(sfunc->Ssize);
    }
}

/*********************************************
 * Put out symbols that define the beginning/end of the .deh_eh section.
 * This gets called if this is the module with "main()" in it.
 */

void Obj::ehsections()
{
    //printf("Obj::ehsections()\n");
}

/*********************************
 * Setup for Symbol s to go into a COMDAT segment.
 * Output (if s is a function):
 *      cseg            segment index of new current code segment
 *      Offset(cseg)         starting offset in cseg
 * Returns:
 *      "segment index" of COMDAT
 */

int Obj::comdatsize(Symbol *s, targ_size_t symsize)
{
    return Obj::comdat(s);
}

int Obj::comdat(Symbol *s)
{
    const char *sectname;
    const char *segname;
    int align;
    int flags;

    //printf("Obj::comdat(Symbol* %s)\n",s->Sident);
    //symbol_print(s);
    symbol_debug(s);

    if (tyfunc(s->ty()))
    {
        sectname = "__textcoal_nt";
        segname = "__TEXT";
        align = 2;              // 4 byte alignment
        flags = S_COALESCED | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
        s->Sseg = MachObj::getsegment(sectname, segname, align, flags);
    }
    else if ((s->ty() & mTYLINK) == mTYthread)
    {
        s->Sfl = FLtlsdata;
        align = 4;
        if (I64)
            s->Sseg = tlsseg()->SDseg;
        else
            s->Sseg = MachObj::getsegment("__tlscoal_nt", "__DATA", align, S_COALESCED);
        Obj::data_start(s, 1 << align, s->Sseg);
    }
    else
    {
        s->Sfl = FLdata;
        sectname = "__datacoal_nt";
        segname = "__DATA";
        align = 4;              // 16 byte alignment
        s->Sseg = MachObj::getsegment(sectname, segname, align, S_COALESCED);
        Obj::data_start(s, 1 << align, s->Sseg);
    }
                                // find or create new segment
    if (s->Salignment > (1 << align))
        SegData[s->Sseg]->SDalignment = s->Salignment;
    s->Soffset = SegData[s->Sseg]->SDoffset;
    if (s->Sfl == FLdata || s->Sfl == FLtlsdata)
    {   // Code symbols are 'published' by Obj::func_start()

        Obj::pubdef(s->Sseg,s,s->Soffset);
        searchfixlist(s);               // backpatch any refs to this symbol
    }
    return s->Sseg;
}

int Obj::readonly_comdat(Symbol *s)
{
    assert(0);
    return 0;
}

/***********************************
 * Returns:
 *      jump table segment for function s
 */
int Obj::jmpTableSegment(Symbol *s)
{
    return (config.flags & CFGromable) ? cseg : CDATA;
}

/**********************************
 * Get segment.
 * Input:
 *      align   segment alignment as power of 2
 * Returns:
 *      segment index of found or newly created segment
 */

int MachObj::getsegment(const char *sectname, const char *segname,
        int align, int flags)
{
    assert(strlen(sectname) <= 16);
    assert(strlen(segname)  <= 16);
    for (int seg = 1; seg <= seg_count; seg++)
    {   seg_data *pseg = SegData[seg];
        if (I64)
        {
            if (strncmp(SecHdrTab64[pseg->SDshtidx].sectname, sectname, 16) == 0 &&
                strncmp(SecHdrTab64[pseg->SDshtidx].segname, segname, 16) == 0)
                return seg;         // return existing segment
        }
        else
        {
            if (strncmp(SecHdrTab[pseg->SDshtidx].sectname, sectname, 16) == 0 &&
                strncmp(SecHdrTab[pseg->SDshtidx].segname, segname, 16) == 0)
                return seg;         // return existing segment
        }
    }

    int seg = ++seg_count;
    if (seg_count >= seg_max)
    {                           // need more room in segment table
        seg_max += 10;
        SegData = (seg_data **)mem_realloc(SegData,seg_max * sizeof(seg_data *));
        memset(&SegData[seg_count], 0, (seg_max - seg_count) * sizeof(seg_data *));
    }
    assert(seg_count < seg_max);
    if (SegData[seg])
    {   seg_data *pseg = SegData[seg];
        Outbuffer *b1 = pseg->SDbuf;
        Outbuffer *b2 = pseg->SDrel;
        memset(pseg, 0, sizeof(seg_data));
        if (b1)
            b1->setsize(0);
        if (b2)
            b2->setsize(0);
        pseg->SDbuf = b1;
        pseg->SDrel = b2;
    }
    else
    {
        seg_data *pseg = (seg_data *)mem_calloc(sizeof(seg_data));
        SegData[seg] = pseg;
        if (flags != S_ZEROFILL)
        {   pseg->SDbuf = new Outbuffer(4096);
            pseg->SDbuf->reserve(4096);
        }
    }

    //dbg_printf("\tNew segment - %d size %d\n", seg,SegData[seg]->SDbuf);
    seg_data *pseg = SegData[seg];

    pseg->SDseg = seg;
    pseg->SDoffset = 0;

    if (I64)
    {
        struct section_64 *sec = (struct section_64 *)
            SECbuf->writezeros(sizeof(struct section_64));
        strncpy(sec->sectname, sectname, 16);
        strncpy(sec->segname, segname, 16);
        sec->align = align;
        sec->flags = flags;
    }
    else
    {
        struct section *sec = (struct section *)
            SECbuf->writezeros(sizeof(struct section));
        strncpy(sec->sectname, sectname, 16);
        strncpy(sec->segname, segname, 16);
        sec->align = align;
        sec->flags = flags;
    }

    pseg->SDshtidx = section_cnt++;
    pseg->SDaranges_offset = 0;
    pseg->SDlinnum_count = 0;

    //printf("seg_count = %d\n", seg_count);
    return seg;
}

/**********************************
 * Reset code seg to existing seg.
 * Used after a COMDAT for a function is done.
 */

void Obj::setcodeseg(int seg)
{
    cseg = seg;
}

/********************************
 * Define a new code segment.
 * Input:
 *      name            name of segment, if NULL then revert to default
 *      suffix  0       use name as is
 *              1       append "_TEXT" to name
 * Output:
 *      cseg            segment index of new current code segment
 *      Offset(cseg)         starting offset in cseg
 * Returns:
 *      segment index of newly created code segment
 */

int Obj::codeseg(char *name,int suffix)
{
    //dbg_printf("Obj::codeseg(%s,%x)\n",name,suffix);
#if 0
    const char *sfx = (suffix) ? "_TEXT" : NULL;

    if (!name)                          // returning to default code segment
    {
        if (cseg != CODE)               // not the current default
        {
            SegData[cseg]->SDoffset = Offset(cseg);
            Offset(cseg) = SegData[CODE]->SDoffset;
            cseg = CODE;
        }
        return cseg;
    }

    int seg = ElfObj::getsegment(name, sfx, SHT_PROGDEF, SHF_ALLOC|SHF_EXECINSTR, 4);
                                    // find or create code segment

    cseg = seg;                         // new code segment index
    Offset(cseg) = 0;
    return seg;
#else
    return 0;
#endif
}

/*********************************
 * Define segments for Thread Local Storage for 32bit.
 * Output:
 *      seg_tlsseg      set to segment number for TLS segment.
 * Returns:
 *      segment for TLS segment
 */

seg_data *Obj::tlsseg()
{
    //printf("Obj::tlsseg(\n");
    assert(I32);

    if (seg_tlsseg == UNKNOWN)
        seg_tlsseg = MachObj::getsegment("__tls_data", "__DATA", 2, S_REGULAR);
    return SegData[seg_tlsseg];
}


/*********************************
 * Define segments for Thread Local Storage.
 * Output:
 *      seg_tlsseg_bss  set to segment number for TLS segment.
 * Returns:
 *      segment for TLS segment
 */

seg_data *Obj::tlsseg_bss()
{
    assert(I32);

    /* Because DMD does not support native tls for Mach-O 32bit,
     * it's easier to support if we have all the tls in one segment.
     */
    return Obj::tlsseg();
}

/*********************************
 * Define segments for Thread Local Storage data.
 * Output:
 *      seg_tlsseg_data    set to segment number for TLS data segment.
 * Returns:
 *      segment for TLS data segment
 */

seg_data *Obj::tlsseg_data()
{
    //printf("Obj::tlsseg_data(\n");
    assert(I64);

    // The alignment should actually be alignment of the largest variable in
    // the section, but this seems to work anyway.
    if (seg_tlsseg_data == UNKNOWN)
        seg_tlsseg_data = MachObj::getsegment("__thread_data", "__DATA", 4, S_THREAD_LOCAL_REGULAR);
    return SegData[seg_tlsseg_data];
}

/*******************************
 * Output an alias definition record.
 */

void Obj::_alias(const char *n1,const char *n2)
{
    //printf("Obj::_alias(%s,%s)\n",n1,n2);
    assert(0);
#if NOT_DONE
    unsigned len;
    char *buffer;

    buffer = (char *) alloca(strlen(n1) + strlen(n2) + 2 * ONS_OHD);
    len = obj_namestring(buffer,n1);
    len += obj_namestring(buffer + len,n2);
    objrecord(ALIAS,buffer,len);
#endif
}

char *unsstr (unsigned value)
{
    static char buffer [64];

    sprintf (buffer, "%d", value);
    return buffer;
}

/*******************************
 * Mangle a name.
 * Returns:
 *      mangled name
 */

char *obj_mangle2(Symbol *s,char *dest)
{
    size_t len;
    char *name;

    //printf("Obj::mangle(s = %p, '%s'), mangle = x%x\n",s,s->Sident,type_mangle(s->Stype));
    symbol_debug(s);
    assert(dest);
#if SCPP
    name = CPP ? cpp_mangle(s) : s->Sident;
#elif MARS
    name = cpp_mangle(s);
#else
    name = s->Sident;
#endif
    len = strlen(name);                 // # of bytes in name
    //dbg_printf("len %d\n",len);
    switch (type_mangle(s->Stype))
    {
        case mTYman_pas:                // if upper case
        case mTYman_for:
            if (len >= DEST_LEN)
                dest = (char *)mem_malloc(len + 1);
            memcpy(dest,name,len + 1);  // copy in name and ending 0
            for (char *p = dest; *p; p++)
                *p = toupper(*p);
            break;
        case mTYman_std:
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
            if (tyfunc(s->ty()) && !variadic(s->Stype))
#else
            if (!(config.flags4 & CFG4oldstdmangle) &&
                config.exe == EX_WIN32 && tyfunc(s->ty()) &&
                !variadic(s->Stype))
#endif
            {
                char *pstr = unsstr(type_paramsize(s->Stype));
                size_t pstrlen = strlen(pstr);
                size_t destlen = len + 1 + pstrlen + 1;

                if (destlen > DEST_LEN)
                    dest = (char *)mem_malloc(destlen);
                memcpy(dest,name,len);
                dest[len] = '@';
                memcpy(dest + 1 + len, pstr, pstrlen + 1);
                break;
            }
        case mTYman_cpp:
        case mTYman_d:
        case mTYman_sys:
        case 0:
            if (len >= DEST_LEN)
                dest = (char *)mem_malloc(len + 1);
            memcpy(dest,name,len+1);// copy in name and trailing 0
            break;

        case mTYman_c:
            if (len >= DEST_LEN - 1)
                dest = (char *)mem_malloc(1 + len + 1);
            dest[0] = '_';
            memcpy(dest + 1,name,len+1);// copy in name and trailing 0
            break;


        default:
#ifdef DEBUG
            printf("mangling %x\n",type_mangle(s->Stype));
            symbol_print(s);
#endif
            printf("%d\n", type_mangle(s->Stype));
            assert(0);
    }
    //dbg_printf("\t %s\n",dest);
    return dest;
}

/*******************************
 * Export a function name.
 */

void Obj::export_symbol(Symbol *s,unsigned argsize)
{
    //dbg_printf("Obj::export_symbol(%s,%d)\n",s->Sident,argsize);
}

/*******************************
 * Update data information about symbol
 *      align for output and assign segment
 *      if not already specified.
 *
 * Input:
 *      sdata           data symbol
 *      datasize        output size
 *      seg             default seg if not known
 * Returns:
 *      actual seg
 */

int Obj::data_start(Symbol *sdata, targ_size_t datasize, int seg)
{
    targ_size_t alignbytes;

    //printf("Obj::data_start(%s,size %llu,seg %d)\n",sdata->Sident,datasize,seg);
    //symbol_print(sdata);

    assert(sdata->Sseg);
    if (sdata->Sseg == UNKNOWN) // if we don't know then there
        sdata->Sseg = seg;      // wasn't any segment override
    else
        seg = sdata->Sseg;
    targ_size_t offset = Offset(seg);
    if (sdata->Salignment > 0)
    {   if (SegData[seg]->SDalignment < sdata->Salignment)
            SegData[seg]->SDalignment = sdata->Salignment;
        alignbytes = ((offset + sdata->Salignment - 1) & ~(sdata->Salignment - 1)) - offset;
    }
    else
        alignbytes = _align(datasize, offset) - offset;
    if (alignbytes)
        Obj::lidata(seg, offset, alignbytes);
    sdata->Soffset = offset + alignbytes;
    return seg;
}

/*******************************
 * Update function info before codgen
 *
 * If code for this function is in a different segment
 * than the current default in cseg, switch cseg to new segment.
 */

void Obj::func_start(Symbol *sfunc)
{
    //printf("Obj::func_start(%s)\n",sfunc->Sident);
    symbol_debug(sfunc);

    assert(sfunc->Sseg);
    if (sfunc->Sseg == UNKNOWN)
        sfunc->Sseg = CODE;
    //printf("sfunc->Sseg %d CODE %d cseg %d Coffset x%x\n",sfunc->Sseg,CODE,cseg,Offset(cseg));
    cseg = sfunc->Sseg;
    assert(cseg == CODE || cseg > UDATA);
    Obj::pubdef(cseg, sfunc, Offset(cseg));
    sfunc->Soffset = Offset(cseg);

    dwarf_func_start(sfunc);
}

/*******************************
 * Update function info after codgen
 */

void Obj::func_term(Symbol *sfunc)
{
    //dbg_printf("Obj::func_term(%s) offset %x, Coffset %x symidx %d\n",
//          sfunc->Sident, sfunc->Soffset,Offset(cseg),sfunc->Sxtrnnum);

#if 0
    // fill in the function size
    if (I64)
        SymbolTable64[sfunc->Sxtrnnum].st_size = Offset(cseg) - sfunc->Soffset;
    else
        SymbolTable[sfunc->Sxtrnnum].st_size = Offset(cseg) - sfunc->Soffset;
#endif
    dwarf_func_term(sfunc);
}

/********************************
 * Output a public definition.
 * Input:
 *      seg =           segment index that symbol is defined in
 *      s ->            symbol
 *      offset =        offset of name within segment
 */

void Obj::pubdefsize(int seg, Symbol *s, targ_size_t offset, targ_size_t symsize)
{
    return Obj::pubdef(seg, s, offset);
}

void Obj::pubdef(int seg, Symbol *s, targ_size_t offset)
{
#if 0
    printf("Obj::pubdef(%d:x%x s=%p, %s)\n", seg, offset, s, s->Sident);
    //symbol_print(s);
#endif
    symbol_debug(s);

    s->Soffset = offset;
    s->Sseg = seg;
    switch (s->Sclass)
    {
        case SCglobal:
        case SCinline:
            public_symbuf->write(&s, sizeof(s));
            break;
        case SCcomdat:
        case SCcomdef:
            public_symbuf->write(&s, sizeof(s));
            break;
        default:
            local_symbuf->write(&s, sizeof(s));
            break;
    }
    //printf("%p\n", *(void**)public_symbuf->buf);
    s->Sxtrnnum = 1;
}

/*******************************
 * Output an external symbol for name.
 * Input:
 *      name    Name to do EXTDEF on
 *              (Not to be mangled)
 * Returns:
 *      Symbol table index of the definition
 *      NOTE: Numbers will not be linear.
 */

int Obj::external_def(const char *name)
{
    //printf("Obj::external_def('%s')\n",name);
    assert(name);
    assert(extdef == 0);
    extdef = Obj::addstr(symtab_strings, name);
    return 0;
}


/*******************************
 * Output an external for existing symbol.
 * Input:
 *      s       Symbol to do EXTDEF on
 *              (Name is to be mangled)
 * Returns:
 *      Symbol table index of the definition
 *      NOTE: Numbers will not be linear.
 */

int Obj::external(Symbol *s)
{
    //printf("Obj::external('%s') %x\n",s->Sident,s->Svalue);
    symbol_debug(s);
    extern_symbuf->write(&s, sizeof(s));
    s->Sxtrnnum = 1;
    return 0;
}

/*******************************
 * Output a common block definition.
 * Input:
 *      p ->    external identifier
 *      size    size in bytes of each elem
 *      count   number of elems
 * Returns:
 *      Symbol table index for symbol
 */

int Obj::common_block(Symbol *s,targ_size_t size,targ_size_t count)
{
    //printf("Obj::common_block('%s', size=%d, count=%d)\n",s->Sident,size,count);
    symbol_debug(s);

    // can't have code or thread local comdef's
    assert(!(s->ty() & (mTYcs | mTYthread)));

    struct Comdef comdef;
    comdef.sym = s;
    comdef.size = size;
    comdef.count = count;
    comdef_symbuf->write(&comdef, sizeof(comdef));
    s->Sxtrnnum = 1;
    if (!s->Sseg)
        s->Sseg = UDATA;
    return 0;           // should return void
}

int Obj::common_block(Symbol *s, int flag, targ_size_t size, targ_size_t count)
{
    return common_block(s, size, count);
}

/***************************************
 * Append an iterated data block of 0s.
 * (uninitialized data only)
 */

void Obj::write_zeros(seg_data *pseg, targ_size_t count)
{
    Obj::lidata(pseg->SDseg, pseg->SDoffset, count);
}

/***************************************
 * Output an iterated data block of 0s.
 *
 *      For boundary alignment and initialization
 */

void Obj::lidata(int seg,targ_size_t offset,targ_size_t count)
{
    //printf("Obj::lidata(%d,%x,%d)\n",seg,offset,count);
    size_t idx = SegData[seg]->SDshtidx;
    if ((I64 ? SecHdrTab64[idx].flags : SecHdrTab[idx].flags) == S_ZEROFILL)
    {   // Use SDoffset to record size of bss section
        SegData[seg]->SDoffset += count;
    }
    else
    {
        Obj::bytes(seg, offset, count, NULL);
    }
}

/***********************************
 * Append byte to segment.
 */

void Obj::write_byte(seg_data *pseg, unsigned byte)
{
    Obj::_byte(pseg->SDseg, pseg->SDoffset, byte);
}

/************************************
 * Output byte to object file.
 */

void Obj::_byte(int seg,targ_size_t offset,unsigned byte)
{
    Outbuffer *buf = SegData[seg]->SDbuf;
    int save = buf->size();
    //dbg_printf("Obj::_byte(seg=%d, offset=x%lx, byte=x%x)\n",seg,offset,byte);
    buf->setsize(offset);
    buf->writeByte(byte);
    if (save > offset+1)
        buf->setsize(save);
    else
        SegData[seg]->SDoffset = offset+1;
    //dbg_printf("\tsize now %d\n",buf->size());
}

/***********************************
 * Append bytes to segment.
 */

void Obj::write_bytes(seg_data *pseg, unsigned nbytes, void *p)
{
    Obj::bytes(pseg->SDseg, pseg->SDoffset, nbytes, p);
}

/************************************
 * Output bytes to object file.
 * Returns:
 *      nbytes
 */

unsigned Obj::bytes(int seg, targ_size_t offset, unsigned nbytes, void *p)
{
#if 0
    if (!(seg >= 0 && seg <= seg_count))
    {   printf("Obj::bytes: seg = %d, seg_count = %d\n", seg, seg_count);
        *(char*)0=0;
    }
#endif
    assert(seg >= 0 && seg <= seg_count);
    Outbuffer *buf = SegData[seg]->SDbuf;
    if (buf == NULL)
    {
        //dbg_printf("Obj::bytes(seg=%d, offset=x%llx, nbytes=%d, p=%p)\n", seg, offset, nbytes, p);
        //raise(SIGSEGV);
if (!buf) halt();
        assert(buf != NULL);
    }
    int save = buf->size();
    //dbg_printf("Obj::bytes(seg=%d, offset=x%lx, nbytes=%d, p=x%x)\n",
            //seg,offset,nbytes,p);
    buf->position(offset, nbytes);
    if (p)
    {
        buf->writen(p,nbytes);
    }
    else
    {   // Zero out the bytes
        buf->clearn(nbytes);
    }
    if (save > offset+nbytes)
        buf->setsize(save);
    else
        SegData[seg]->SDoffset = offset+nbytes;
    return nbytes;
}

/*********************************************
 * Add a relocation entry for seg/offset.
 */

void MachObj::addrel(int seg, targ_size_t offset, symbol *targsym,
        unsigned targseg, int rtype, int val)
{
    Relocation rel;
    rel.offset = offset;
    rel.targsym = targsym;
    rel.targseg = targseg;
    rel.rtype = rtype;
    rel.flag = 0;
    rel.funcsym = funcsym_p;
    rel.val = val;
    seg_data *pseg = SegData[seg];
    if (!pseg->SDrel)
        pseg->SDrel = new Outbuffer();
    pseg->SDrel->write(&rel, sizeof(rel));
}

/****************************************
 * Sort the relocation entry buffer.
 */

#if __DMC__
static int __cdecl rel_fp(const void *e1, const void *e2)
{   Relocation *r1 = (Relocation *)e1;
    Relocation *r2 = (Relocation *)e2;

    return r1->offset - r2->offset;
}
#else
extern "C" {
static int rel_fp(const void *e1, const void *e2)
{   Relocation *r1 = (Relocation *)e1;
    Relocation *r2 = (Relocation *)e2;

    return r1->offset - r2->offset;
}
}
#endif

void mach_relsort(Outbuffer *buf)
{
    qsort(buf->buf, buf->size() / sizeof(Relocation), sizeof(Relocation), &rel_fp);
}

/*******************************
 * Output a relocation entry for a segment
 * Input:
 *      seg =           where the address is going
 *      offset =        offset within seg
 *      type =          ELF relocation type
 *      index =         Related symbol table index
 *      val =           addend or displacement from address
 */

void ElfObj::addrel(int seg, targ_size_t offset, unsigned type,
                                        IDXSYM symidx, targ_size_t val)
{
}

/*******************************
 * Refer to address that is in the data segment.
 * Input:
 *      seg:offset =    the address being fixed up
 *      val =           displacement from start of target segment
 *      targetdatum =   target segment number (DATA, CDATA or UDATA, etc.)
 *      flags =         CFoff, CFseg
 * Example:
 *      int *abc = &def[3];
 *      to allocate storage:
 *              Obj::reftodatseg(DATA,offset,3 * sizeof(int *),UDATA);
 */

void Obj::reftodatseg(int seg,targ_size_t offset,targ_size_t val,
        unsigned targetdatum,int flags)
{
    Outbuffer *buf = SegData[seg]->SDbuf;
    int save = buf->size();
    buf->setsize(offset);
#if 0
    printf("Obj::reftodatseg(seg:offset=%d:x%llx, val=x%llx, targetdatum %x, flags %x )\n",
        seg,offset,val,targetdatum,flags);
#endif
    assert(seg != 0);
    if (SegData[seg]->isCode() && SegData[targetdatum]->isCode())
    {
        assert(0);
    }
    MachObj::addrel(seg, offset, NULL, targetdatum, RELaddr);
    if (I64)
    {
        if (flags & CFoffset64)
        {
            buf->write64(val);
            if (save > offset + 8)
                buf->setsize(save);
            return;
        }
    }
    buf->write32(val);
    if (save > offset + 4)
        buf->setsize(save);
}

/*******************************
 * Refer to address that is in the current function code (funcsym_p).
 * Only offsets are output, regardless of the memory model.
 * Used to put values in switch address tables.
 * Input:
 *      seg =           where the address is going (CODE or DATA)
 *      offset =        offset within seg
 *      val =           displacement from start of this module
 */

void Obj::reftocodeseg(int seg,targ_size_t offset,targ_size_t val)
{
    //printf("Obj::reftocodeseg(seg=%d, offset=x%lx, val=x%lx )\n",seg,(unsigned long)offset,(unsigned long)val);
    assert(seg > 0);
    Outbuffer *buf = SegData[seg]->SDbuf;
    int save = buf->size();
    buf->setsize(offset);
    val -= funcsym_p->Soffset;
    MachObj::addrel(seg, offset, funcsym_p, 0, RELaddr);
//    if (I64)
//        buf->write64(val);
//    else
        buf->write32(val);
    if (save > offset + 4)
        buf->setsize(save);
}

/*******************************
 * Refer to an identifier.
 * Input:
 *      seg =   where the address is going (CODE or DATA)
 *      offset =        offset within seg
 *      s ->            Symbol table entry for identifier
 *      val =           displacement from identifier
 *      flags =         CFselfrel: self-relative
 *                      CFseg: get segment
 *                      CFoff: get offset
 *                      CFpc32: [RIP] addressing, val is 0, -1, -2 or -4
 *                      CFoffset64: 8 byte offset for 64 bit builds
 * Returns:
 *      number of bytes in reference (4 or 8)
 */

int Obj::reftoident(int seg, targ_size_t offset, Symbol *s, targ_size_t val,
        int flags)
{
    int retsize = (flags & CFoffset64) ? 8 : 4;
#if 0
    dbg_printf("\nObj::reftoident('%s' seg %d, offset x%llx, val x%llx, flags x%x)\n",
        s->Sident,seg,(unsigned long long)offset,(unsigned long long)val,flags);
    printf("retsize = %d\n", retsize);
    //dbg_printf("Sseg = %d, Sxtrnnum = %d\n",s->Sseg,s->Sxtrnnum);
    symbol_print(s);
#endif
    assert(seg > 0);
    if (s->Sclass != SClocstat && !s->Sxtrnnum)
    {   // It may get defined later as public or local, so defer
        size_t numbyteswritten = addtofixlist(s, offset, seg, val, flags);
        assert(numbyteswritten == retsize);
    }
    else
    {
        if (I64)
        {
            //if (s->Sclass != SCcomdat)
                //val += s->Soffset;
            int v = 0;
            if (flags & CFpc32)
                v = (int)val;
            if (flags & CFselfrel)
            {
                MachObj::addrel(seg, offset, s, 0, RELrel, v);
            }
            else
            {
                MachObj::addrel(seg, offset, s, 0, RELaddr, v);
            }
        }
        else
        {
            if (SegData[seg]->isCode() && flags & CFselfrel)
            {
                if (!jumpTableSeg)
                {
                    jumpTableSeg =
                        MachObj::getsegment("__jump_table", "__IMPORT",  0, S_SYMBOL_STUBS | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS | S_ATTR_SELF_MODIFYING_CODE);
                }
                seg_data *pseg = SegData[jumpTableSeg];
                if (I64)
                    SecHdrTab64[pseg->SDshtidx].reserved2 = 5;
                else
                    SecHdrTab[pseg->SDshtidx].reserved2 = 5;

                if (!indirectsymbuf1)
                    indirectsymbuf1 = new Outbuffer();
                else
                {   // Look through indirectsym to see if it is already there
                    int n = indirectsymbuf1->size() / sizeof(Symbol *);
                    Symbol **psym = (Symbol **)indirectsymbuf1->buf;
                    for (int i = 0; i < n; i++)
                    {   // Linear search, pretty pathetic
                        if (s == psym[i])
                        {   val = i * 5;
                            goto L1;
                        }
                    }
                }

                val = pseg->SDbuf->size();
                static char halts[5] = { 0xF4,0xF4,0xF4,0xF4,0xF4 };
                pseg->SDbuf->write(halts, 5);

                // Add symbol s to indirectsymbuf1
                indirectsymbuf1->write(&s, sizeof(Symbol *));
             L1:
                val -= offset + 4;
                MachObj::addrel(seg, offset, NULL, jumpTableSeg, RELrel);
            }
            else if (SegData[seg]->isCode() &&
                     !(flags & CFindirect) &&
                    ((s->Sclass != SCextern && SegData[s->Sseg]->isCode()) || s->Sclass == SClocstat || s->Sclass == SCstatic))
            {
                val += s->Soffset;
                MachObj::addrel(seg, offset, NULL, s->Sseg, RELaddr);
            }
            else if ((flags & CFindirect) ||
                     SegData[seg]->isCode() && !tyfunc(s->ty()))
            {
                if (!pointersSeg)
                {
                    pointersSeg =
                        MachObj::getsegment("__pointers", "__IMPORT",  0, S_NON_LAZY_SYMBOL_POINTERS);
                }
                seg_data *pseg = SegData[pointersSeg];

                if (!indirectsymbuf2)
                    indirectsymbuf2 = new Outbuffer();
                else
                {   // Look through indirectsym to see if it is already there
                    int n = indirectsymbuf2->size() / sizeof(Symbol *);
                    Symbol **psym = (Symbol **)indirectsymbuf2->buf;
                    for (int i = 0; i < n; i++)
                    {   // Linear search, pretty pathetic
                        if (s == psym[i])
                        {   val = i * 4;
                            goto L2;
                        }
                    }
                }

                val = pseg->SDbuf->size();
                pseg->SDbuf->writezeros(NPTRSIZE);

                // Add symbol s to indirectsymbuf2
                indirectsymbuf2->write(&s, sizeof(Symbol *));

             L2:
                //printf("Obj::reftoident: seg = %d, offset = x%x, s = %s, val = x%x, pointersSeg = %d\n", seg, (int)offset, s->Sident, (int)val, pointersSeg);
                if (flags & CFindirect)
                {
                    Relocation rel;
                    rel.offset = offset;
                    rel.targsym = NULL;
                    rel.targseg = pointersSeg;
                    rel.rtype = RELaddr;
                    rel.flag = 0;
                    rel.funcsym = NULL;
                    rel.val = 0;
                    seg_data *pseg = SegData[seg];
                    if (!pseg->SDrel)
                        pseg->SDrel = new Outbuffer();
                    pseg->SDrel->write(&rel, sizeof(rel));
                }
                else
                    MachObj::addrel(seg, offset, NULL, pointersSeg, RELaddr);
            }
            else
            {   //val -= s->Soffset;
                MachObj::addrel(seg, offset, s, 0, RELaddr);
            }
        }

        Outbuffer *buf = SegData[seg]->SDbuf;
        int save = buf->size();
        buf->position(offset, retsize);
        //printf("offset = x%llx, val = x%llx\n", offset, val);
        if (retsize == 8)
            buf->write64(val);
        else
            buf->write32(val);
        if (save > offset + retsize)
            buf->setsize(save);
    }
    return retsize;
}

/*****************************************
 * Generate far16 thunk.
 * Input:
 *      s       Symbol to generate a thunk for
 */

void Obj::far16thunk(Symbol *s)
{
    //dbg_printf("Obj::far16thunk('%s')\n", s->Sident);
    assert(0);
}

/**************************************
 * Mark object file as using floating point.
 */

void Obj::fltused()
{
    //dbg_printf("Obj::fltused()\n");
}

/************************************
 * Close and delete .OBJ file.
 */

void objfile_delete()
{
    //remove(fobjname); // delete corrupt output file
}

/**********************************
 * Terminate.
 */

void objfile_term()
{
#if TERMCODE
    mem_free(fobjname);
    fobjname = NULL;
#endif
}

/**********************************
  * Write to the object file
  */
void objfile_write(FILE *fd, void *buffer, unsigned len)
{
    fobjbuf->write(buffer, len);
}

long elf_align(targ_size_t size, long foffset)
{
    if (size <= 1)
        return foffset;
    long offset = (foffset + size - 1) & ~(size - 1);
    if (offset > foffset)
        fobjbuf->writezeros(offset - foffset);
    return offset;
}

/***************************************
 * Stuff pointer to ModuleInfo in its own segment.
 */

#if MARS

void Obj::moduleinfo(Symbol *scc)
{
    int align = I64 ? 3 : 2; // align to NPTRSIZE

    int seg = MachObj::getsegment("__minfodata", "__DATA", align, S_REGULAR);
    //printf("Obj::moduleinfo(%s) seg = %d:x%x\n", scc->Sident, seg, Offset(seg));

#if 0
    type *t = type_fake(TYint);
    t->Tmangle = mTYman_c;
    char *p = (char *)malloc(5 + strlen(scc->Sident) + 1);
    strcpy(p, "SUPER");
    strcpy(p + 5, scc->Sident);
    symbol *s_minfo_beg = symbol_name(p, SCglobal, t);
    Obj::pubdef(seg, s_minfo_beg, 0);
#endif

    int flags = CFoff;
    if (I64)
        flags |= CFoffset64;
    SegData[seg]->SDoffset += Obj::reftoident(seg, Offset(seg), scc, 0, flags);
}

#endif

/*************************************
 */

void Obj::gotref(symbol *s)
{
    //printf("Obj::gotref(%x '%s', %d)\n",s,s->Sident, s->Sclass);
    switch(s->Sclass)
    {
        case SCstatic:
        case SClocstat:
            s->Sfl = FLgotoff;
            break;

        case SCextern:
        case SCglobal:
        case SCcomdat:
        case SCcomdef:
            s->Sfl = FLgot;
            break;

        default:
            break;
    }
}

/**
 * Returns the symbol for the __tlv_bootstrap function.
 *
 * This function is used in the implementation of native thread local storage.
 * It's used as a placeholder in the TLV descriptors. The dynamic linker will
 * replace the placeholder with a real function at load time.
 */
symbol *Obj::tlv_bootstrap()
{
    if (!tlv_bootstrap_sym)
        tlv_bootstrap_sym = symbol_name("__tlv_bootstrap", SCextern, type_fake(TYnfunc));
    return tlv_bootstrap_sym;
}


void Obj::write_pointerRef(Symbol* s, unsigned off)
{
}

/*********************************
 * Define segments for Thread Local Storage variables.
 * Output:
 *      seg_tlsseg      set to segment number for TLS variables segment.
 * Returns:
 *      segment for TLS variables segment
 */

seg_data *MachObj64::tlsseg()
{
    //printf("MachObj64::tlsseg(\n");

    if (seg_tlsseg == UNKNOWN)
        seg_tlsseg = MachObj::getsegment("__thread_vars", "__DATA", 0, S_THREAD_LOCAL_VARIABLES);
    return SegData[seg_tlsseg];
}

/*********************************
 * Define segments for Thread Local Storage.
 * Output:
 *      seg_tlsseg_bss  set to segment number for TLS data segment.
 * Returns:
 *      segment for TLS segment
 */

seg_data *MachObj64::tlsseg_bss()
{
    //printf("MachObj64::tlsseg_bss(\n");

    // The alignment should actually be alignment of the largest variable in
    // the section, but this seems to work anyway.
    if (seg_tlsseg_bss == UNKNOWN)
        seg_tlsseg_bss = MachObj::getsegment("__thread_bss", "__DATA", 3, S_THREAD_LOCAL_ZEROFILL);
    return SegData[seg_tlsseg_bss];
}

/******************************************
 * Generate fixup specific to .eh_frame and .gcc_except_table sections.
 * Params:
 *      seg = segment of where to write fixup
 *      offset = offset of where to write fixup
 *      s = fixup is a reference to this Symbol
 *      val = displacement from s
 * Returns:
 *      number of bytes written at seg:offset
 */
int dwarf_reftoident(int seg, targ_size_t offset, Symbol *s, targ_size_t val)
{
    //printf("dwarf_reftoident(seg=%d offset=x%x s=%s val=x%x\n", seg, (int)offset, s->Sident, (int)val);
    MachObj::reftoident(seg, offset, s, val + 4, I64 ? CFoff : CFindirect);
    return 4;
}

/*****************************************
 * Generate LSDA and PC_Begin fixups in the __eh_frame segment encoded as DW_EH_PE_pcrel|ptr.
 * 64 bits
 *   LSDA
 *      [0] address x0071 symbolnum 6 pcrel 0 length 3 extern 1 type 5 RELOC_SUBTRACTOR __Z3foov.eh
 *      [1] address x0071 symbolnum 1 pcrel 0 length 3 extern 1 type 0 RELOC_UNSIGNED   GCC_except_table2
 *   PC_Begin:
 *      [2] address x0060 symbolnum 6 pcrel 0 length 3 extern 1 type 5 RELOC_SUBTRACTOR __Z3foov.eh
 *      [3] address x0060 symbolnum 5 pcrel 0 length 3 extern 1 type 0 RELOC_UNSIGNED   __Z3foov
 *      Want the result to be  &s - pc
 *      The fixup yields       &s - &fdesym + value
 *      Therefore              value = &fdesym - pc
 *      which is the same as   fdesym->Soffset - offset
 * 32 bits
 *   LSDA
 *      [6] address x0028 pcrel 0 length 2 value x0 type 4 RELOC_LOCAL_SECTDIFF
 *      [7] address x0000 pcrel 0 length 2 value x1dc type 1 RELOC_PAIR
 *   PC_Begin
 *      [8] address x0013 pcrel 0 length 2 value x228 type 4 RELOC_LOCAL_SECTDIFF
 *      [9] address x0000 pcrel 0 length 2 value x1c7 type 1 RELOC_PAIR
 * Params:
 *      dfseg = segment of where to write fixup (eh_frame segment)
 *      offset = offset of where to write fixup (eh_frame offset)
 *      s = fixup is a reference to this Symbol (GCC_except_table%d or function_name)
 *      val = displacement from s
 *      fdesym = function_name.eh
 * Returns:
 *      number of bytes written at seg:offset
 */
int dwarf_eh_frame_fixup(int dfseg, targ_size_t offset, Symbol *s, targ_size_t val, Symbol *fdesym)
{
    Outbuffer *buf = SegData[dfseg]->SDbuf;
    assert(offset == buf->size());
    assert(fdesym->Sseg == dfseg);
    if (I64)
        buf->write64(val);  // add in 'value' later
    else
        buf->write32(val);

    Relocation rel;
    rel.offset = offset;
    rel.targsym = s;
    rel.targseg = 0;
    rel.rtype = RELaddr;
    rel.flag = 1;
    rel.funcsym = fdesym;
    rel.val = 0;
    seg_data *pseg = SegData[dfseg];
    if (!pseg->SDrel)
        pseg->SDrel = new Outbuffer();
    pseg->SDrel->write(&rel, sizeof(rel));

    return I64 ? 8 : 4;
}

#endif
#endif
