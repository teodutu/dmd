module dmd.lowering;

import core.stdc.stdio;

import dmd.arraytypes;
import dmd.dsymbol;
import dmd.dsymbolsem;
import dmd.errors;
import dmd.dscope;
import dmd.expression;
import dmd.expressionsem;
import dmd.globals;
import dmd.id;
import dmd.identifier;
import dmd.location;
import dmd.mtype;
import dmd.root.string;
import dmd.rootobject;
import dmd.tokens;


struct Lowered
{
    Expression e;
    Scope* sc;
}

static Lowered[] needsLowering;

void addLowering(Expression e, Scope* sc)
{
    if (auto ale = e.isArrayLiteralExp())
        if (ale.ownedByCtfe != OwnedBy.code)
            return;

    sc.setNoFree();
    needsLowering ~= Lowered(e, sc);
}

Scope* removeLowering(Expression e)
{
    foreach (i, ne; needsLowering)
    {
        if (ne.e == e)
        {
            // needsLowering = needsLowering[0 .. i] ~ needsLowering[i + 1 .. $];
            return ne.sc;
        }
    }

    // printf("Not found: %s; loc = %s\n", e.toChars(), e.loc.toChars());
    return null;
    // assert(0);
}

void replaceLowering(Expression old, Expression newExp)
{
    Scope* sc = removeLowering(old);
    if (sc)
        addLowering(newExp, sc);
}

void lowerExpressions()
{
    foreach (ne; needsLowering)
    {
        // printf("lower sc = %p\n", ne.sc);
        if (!ne.sc)
            printf("no scope: ne.e = %s; loc = \n", ne.e.toChars(), ne.e.loc.toChars());
        else
            lower(ne.e, ne.sc);
    }
}

void lower(Expression e, Scope* sc)
{
    // Scope *s = sc;
    // while(s)
    // {
    //     printf("s = %p\n", s);
    //     s = s.enclosing;
    // }

    void lowerArrayLiteral(ArrayLiteralExp e)
    {
	//if (!e.type)
	//	printf("null type; e = %s; loc = %s\n", e.toChars(), e.loc.toChars());
    	// printf("loc = %s; type = %p\n", e.loc.toChars(), e.type);
	// printf("e = %s; loc = %s; type = %p\n", e.toChars(), e.loc.toChars(), e.type);
        // if (!e.onstack && tb.ty != Tsarray && !(sc.flags & SCOPE.Cfile && tb.ty == Tpointer) &&
        // printf("exp = %s; loc = %s; codegen = %d\n", e.toChars(), e.loc.toChars(), sc.needsCodegen());
        // if (length && sc.needsCodegen())
        // printf("lowering %s\n", e.toChars());
        if (!global.params.betterC && !e.ownedByCtfe && !(sc.flags & SCOPE.Cfile))
        {
            Type tb = e.type.toBasetype();
            const length = e.elements ? e.elements.length : 0;
            // printf("inside if\n");
            auto hook = global.params.tracegc ? Id._d_arrayliteralTXTrace : Id._d_arrayliteralTX;
            if (!verifyHookExist(e.loc, *sc, hook, "array literal"))
                return;

            // printf("array literal exp = %s\n", e.toChars());

            /* Lower the memory allocation and initialization of `[x1, x2, ..., xn]`
             * to `_d_arrayliteralTX!T(n)`.
             */
            Expression lowering = new IdentifierExp(e.loc, Id.empty);
            lowering = new DotIdExp(e.loc, lowering, Id.object);
            auto tiargs = new Objects();
            /* Remove `inout`, `const`, `immutable` and `shared` to reduce
             * the number of generated `_d_arrayliteralTX` instances.
             */
            // bool isShared;
            // tiargs.push(removeTypeQualifiers(e.type.nextOf(), isShared));
            tiargs.push(e.type);
            // printf("type = %s\n", e.type.toChars());
            lowering = new DotTemplateInstanceExp(e.loc, lowering, hook, tiargs);

            auto arguments = new Expressions();
            if (global.params.tracegc)
            {
                auto funcname = (sc.callsc && sc.callsc.func) ?
                    sc.callsc.func.toPrettyChars() : (sc.func ? sc.func.toPrettyChars() : sc._module.toPrettyChars());
                arguments.push(new StringExp(e.loc, e.loc.filename.toDString()));
                arguments.push(new IntegerExp(e.loc, e.loc.linnum, Type.tint32));
                arguments.push(new StringExp(e.loc, funcname.toDString()));
            }
            arguments.push(new IntegerExp(e.loc, length, Type.tsize_t));
            // arguments.push(new IntegerExp(e.loc, isShared, Type.tbool));

            lowering = new CallExp(e.loc, lowering, arguments);
            // printf("lowering before semantic = %s\n", lowering.toChars());
            e.lowering = lowering.expressionSemantic(sc);
            // printf("lowering of %s = %s\n", e.toChars(), e.lowering.toChars());
        }
    }

    switch (e.op)
    {
        default:                    return;
        case EXP.arrayLiteral:      lowerArrayLiteral(e.isArrayLiteralExp());
    }
}

/*******************************
 * Make sure that the runtime hook `id` exists.
 * Params:
 *      loc = location to use for error messages
 *      sc = current scope
 *      id = the hook identifier
 *      description = what the hook does
 *      module_ = what module the hook is located in
 * Returns:
 *      a `bool` indicating if the hook is present.
 */
bool verifyHookExist(const ref Loc loc, ref Scope sc, Identifier id, string description, Identifier module_ = Id.object)
{
    Dsymbol pscopesym;
    auto rootSymbol = sc.search(loc, Id.empty, pscopesym);
    if (auto moduleSymbol = rootSymbol.search(loc, module_))
        if (moduleSymbol.search(loc, id))
          return true;
    error(loc, "`%s.%s` not found. The current runtime does not support %.*s, or the runtime is corrupt.", module_.toChars(), id.toChars(), cast(int)description.length, description.ptr);
    return false;
}
