// Copyright (C) 1984-1998 by Symantec
// Copyright (C) 2000-2012 by Digital Mars
// All Rights Reserved
// http://www.digitalmars.com
// Written by Walter Bright
/*
 * This source file is made available for personal use
 * only. The license is in /dmd/src/dmd/backendlicense.txt
 * or /dm/src/dmd/backendlicense.txt
 * For any other uses, please contact Digital Mars.
 */

#if !SPP

#include        <stdio.h>
#include        <string.h>
#include        <stdlib.h>
#include        <time.h>
#include        "cc.h"
#include        "el.h"
#include        "code.h"
#include        "oper.h"
#include        "global.h"
#include        "type.h"
#include        "tinfo.h"
#if SCPP
#include        "exh.h"
#endif

#if HYDRATE
#include        "parser.h"
#endif

static char __file__[] = __FILE__;      /* for tassert.h                */
#include        "tassert.h"

extern targ_size_t retsize;
STATIC void pinholeopt_unittest();
STATIC void do8bit (enum FL,union evc *);
STATIC void do16bit (enum FL,union evc *,int);
STATIC void do32bit (enum FL,union evc *,int,targ_size_t = 0);
STATIC void do64bit (enum FL,union evc *,int);

static int hasframe;            /* !=0 if this function has a stack frame */
static targ_size_t Foff;        // BP offset of floating register
static targ_size_t CSoff;       // offset of common sub expressions
static targ_size_t NDPoff;      // offset of saved 8087 registers
int BPoff;                      // offset from BP
static int EBPtoESP;            // add to EBP offset to get ESP offset
static int AAoff;               // offset of alloca temporary

#if ELFOBJ || MACHOBJ
#define JMPSEG  CDATA
#define JMPOFF  CDoffset
#else
#define JMPSEG  DATA
#define JMPOFF  Doffset
#endif

/*************
 * Size in bytes of each instruction.
 * 0 means illegal instruction.
 * bit  M:      if there is a modregrm field (EV1 is reserved for modregrm)
 * bit  T:      if there is a second operand (EV2)
 * bit  E:      if second operand is only 8 bits
 * bit  A:      a short version exists for the AX reg
 * bit  R:      a short version exists for regs
 * bits 2..0:   size of instruction (excluding optional bytes)
 */

#define M 0x80
#define T 0x40
#define E 0x20
#define A 0x10
#define R 0x08
#define W 0

static unsigned char inssize[256] =
{       M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 00 */
        M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 08 */
        M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 10 */
        M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 18 */
        M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 20 */
        M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 28 */
        M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 30 */
        M|2,M|2,M|2,M|2,        T|E|2,T|3,1,1,          /* 38 */
        1,1,1,1,                1,1,1,1,                /* 40 */
        1,1,1,1,                1,1,1,1,                /* 48 */
        1,1,1,1,                1,1,1,1,                /* 50 */
        1,1,1,1,                1,1,1,1,                /* 58 */
        1,1,M|2,M|2,            1,1,1,1,                /* 60 */
        T|3,M|T|4,T|E|2,M|T|E|3, 1,1,1,1,               /* 68 */
        T|E|2,T|E|2,T|E|2,T|E|2, T|E|2,T|E|2,T|E|2,T|E|2,       /* 70 */
        T|E|2,T|E|2,T|E|2,T|E|2, T|E|2,T|E|2,T|E|2,T|E|2,       /* 78 */
        M|T|E|A|3,M|T|A|4,M|T|E|3,M|T|E|3,      M|2,M|2,M|2,M|A|R|2, /* 80 */
        M|A|2,M|A|2,M|A|2,M|A|2,        M|2,M|2,M|2,M|R|2,      /* 88 */
        1,1,1,1,                1,1,1,1,                /* 90 */
        1,1,T|5,1,              1,1,1,1,                /* 98 */
#if 0 /* cod3_set32() patches this */
        T|5,T|5,T|5,T|5,        1,1,1,1,                /* A0 */
#else
        T|3,T|3,T|3,T|3,        1,1,1,1,                /* A0 */
#endif
        T|E|2,T|3,1,1,          1,1,1,1,                /* A8 */
        T|E|2,T|E|2,T|E|2,T|E|2, T|E|2,T|E|2,T|E|2,T|E|2,       /* B0 */
        T|3,T|3,T|3,T|3,        T|3,T|3,T|3,T|3,                /* B8 */
        M|T|E|3,M|T|E|3,T|3,1,  M|2,M|2,M|T|E|R|3,M|T|R|4,      /* C0 */
        T|E|4,1,T|3,1,          1,T|E|2,1,1,            /* C8 */
        M|2,M|2,M|2,M|2,        T|E|2,T|E|2,0,1,        /* D0 */
        /* For the floating instructions, allow room for the FWAIT      */
        M|2,M|2,M|2,M|2,        M|2,M|2,M|2,M|2,        /* D8 */
        T|E|2,T|E|2,T|E|2,T|E|2, T|E|2,T|E|2,T|E|2,T|E|2,       /* E0 */
        T|3,T|3,T|5,T|E|2,              1,1,1,1,                /* E8 */
        1,0,1,1,                1,1,M|A|2,M|A|2,                /* F0 */
        1,1,1,1,                1,1,M|2,M|R|2                   /* F8 */
};

static const unsigned char inssize32[256] =
{       2,2,2,2,        2,5,1,1,                /* 00 */
        2,2,2,2,        2,5,1,1,                /* 08 */
        2,2,2,2,        2,5,1,1,                /* 10 */
        2,2,2,2,        2,5,1,1,                /* 18 */
        2,2,2,2,        2,5,1,1,                /* 20 */
        2,2,2,2,        2,5,1,1,                /* 28 */
        2,2,2,2,        2,5,1,1,                /* 30 */
        2,2,2,2,        2,5,1,1,                /* 38 */
        1,1,1,1,        1,1,1,1,                /* 40 */
        1,1,1,1,        1,1,1,1,                /* 48 */
        1,1,1,1,        1,1,1,1,                /* 50 */
        1,1,1,1,        1,1,1,1,                /* 58 */
        1,1,2,2,        1,1,1,1,                /* 60 */
        5,6,2,3,        1,1,1,1,                /* 68 */
        2,2,2,2,        2,2,2,2,                /* 70 */
        2,2,2,2,        2,2,2,2,                /* 78 */
        3,6,3,3,        2,2,2,2,                /* 80 */
        2,2,2,2,        2,2,2,2,                /* 88 */
        1,1,1,1,        1,1,1,1,                /* 90 */
        1,1,7,1,        1,1,1,1,                /* 98 */
        5,5,5,5,        1,1,1,1,                /* A0 */
        2,5,1,1,        1,1,1,1,                /* A8 */
        2,2,2,2,        2,2,2,2,                /* B0 */
        5,5,5,5,        5,5,5,5,                /* B8 */
        3,3,3,1,        2,2,3,6,                /* C0 */
        4,1,3,1,        1,2,1,1,                /* C8 */
        2,2,2,2,        2,2,0,1,                /* D0 */
        /* For the floating instructions, don't need room for the FWAIT */
        2,2,2,2,        2,2,2,2,                /* D8 */

        2,2,2,2,        2,2,2,2,                /* E0 */
        5,5,7,2,        1,1,1,1,                /* E8 */
        1,0,1,1,        1,1,2,2,                /* F0 */
        1,1,1,1,        1,1,2,2                 /* F8 */
};

/* For 2 byte opcodes starting with 0x0F        */
static unsigned char inssize2[256] =
{       M|3,M|3,M|3,M|3,        2,2,2,2,                // 00
        2,2,M|3,2,              2,2,2,M|T|E|4,          // 08
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 10
        M|3,2,2,2,              2,2,2,2,                // 18
        M|3,M|3,M|3,M|3,        M|3,2,M|3,2,            // 20
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 28
        2,2,2,2,                2,2,2,2,                // 30
        M|4,2,M|T|E|5,2,        2,2,2,2,                // 38
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 40
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 48
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 50
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 58
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 60
        M|3,M|3,M|3,M|3,        M|3,M|3,M|3,M|3,        // 68
        M|T|E|4,M|T|E|4,M|T|E|4,M|T|E|4, M|3,M|3,M|3,2, // 70
        2,2,2,2,                M|3,M|3,M|3,M|3,        // 78
        W|T|4,W|T|4,W|T|4,W|T|4, W|T|4,W|T|4,W|T|4,W|T|4, // 80
        W|T|4,W|T|4,W|T|4,W|T|4, W|T|4,W|T|4,W|T|4,W|T|4, // 88
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,       // 90
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,       // 98
        2,2,2,M|3,      M|T|E|4,M|3,2,2,        // A0
        2,2,2,M|3,      M|T|E|4,M|3,M|3,M|3,    // A8
        M|E|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,     // B0
        M|3,2,M|T|E|4,M|3, M|3,M|3,M|3,M|3,     // B8
        M|3,M|3,M|T|E|4,M|3, M|T|E|4,M|T|E|4,M|T|E|4,M|3,       // C0
        2,2,2,2,        2,2,2,2,                // C8
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,       // D0
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,       // D8
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,       // E0
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,       // E8
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,M|3,       // F0
        M|3,M|3,M|3,M|3, M|3,M|3,M|3,2          // F8
};

/*************************************************
 * Allocate register temporaries
 */

code *REGSAVE::save(code *c, int reg, unsigned *pidx)
{
    unsigned i;
    if (reg >= XMM0)
    {
        alignment = 16;
        idx = (idx + 15) & ~15;
        i = idx;
        idx += 16;
        // MOVD idx[RBP],xmm
        c = genc1(c,0xF20F11,modregxrm(2, reg - XMM0, BPRM),FLregsave,(targ_uns) i);
    }
    else
    {
        if (!alignment)
            alignment = REGSIZE;
        i = idx;
        idx += REGSIZE;
        // MOV idx[RBP],reg
        c = genc1(c,0x89,modregxrm(2, reg, BPRM),FLregsave,(targ_uns) i);
        if (I64)
            code_orrex(c, REX_W);
    }
    reflocal = TRUE;
    if (idx > top)
        top = idx;              // keep high water mark
    *pidx = i;
    return c;
}

code *REGSAVE::restore(code *c, int reg, unsigned idx)
{
    if (reg >= XMM0)
    {
        assert(alignment == 16);
        // MOVD xmm,idx[RBP]
        c = genc1(c,0xF20F10,modregxrm(2, reg - XMM0, BPRM),FLregsave,(targ_uns) idx);
    }
    else
    {   // MOV reg,idx[RBP]
        c = genc1(c,0x8B,modregxrm(2, reg, BPRM),FLregsave,(targ_uns) idx);
        if (I64)
            code_orrex(c, REX_W);
    }
    return c;
}

/************************************
 * Size for vex encoded instruction.
 */

unsigned char vex_inssize(code *c)
{
    assert(c->Iflags & CFvex);
    unsigned char ins;
    if (c->Iflags & CFvex3)
    {
        switch (c->Ivex.mmmm)
        {
        case 0: // no prefix
        case 1: // 0F
            ins = inssize2[c->Ivex.op] + 2;
            break;
        case 2: // 0F 38
            ins = inssize2[0x38] + 1;
            break;
        case 3: // 0F 3A
            ins = inssize2[0x3A] + 1;
            break;
        default:
            assert(0);
        }
    }
    else
    {
        ins = inssize2[c->Ivex.op] + 1;
    }
    return ins;
}

/************************************
 * Determine if there is a modregrm byte for code.
 */

int cod3_EA(code *c)
{   unsigned ins;

    unsigned op1 = c->Iop & 0xFF;
    if (op1 == ESCAPE)
        ins = 0;
    else if ((c->Iop & 0xFFFD00) == 0x0F3800)
        ins = inssize2[(c->Iop >> 8) & 0xFF];
    else if ((c->Iop & 0xFF00) == 0x0F00)
        ins = inssize2[op1];
    else
        ins = inssize[op1];
    return ins & M;
}

/********************************
 * set initial global variable values
 */

void cod3_setdefault()
{
    fregsaved = mBP | mSI | mDI;
}

/********************************
 * Fix global variables for 386.
 */

void cod3_set32()
{
    inssize[0xA0] = T|5;
    inssize[0xA1] = T|5;
    inssize[0xA2] = T|5;
    inssize[0xA3] = T|5;
    BPRM = 5;                       /* [EBP] addressing mode        */
    fregsaved = mBP | mBX | mSI | mDI;      // saved across function calls
    FLOATREGS = FLOATREGS_32;
    FLOATREGS2 = FLOATREGS2_32;
    DOUBLEREGS = DOUBLEREGS_32;
    if (config.flags3 & CFG3eseqds)
        fregsaved |= mES;

    for (unsigned i = 0x80; i < 0x90; i++)
        inssize2[i] = W|T|6;
}

/********************************
 * Fix global variables for I64.
 */

void cod3_set64()
{
    inssize[0xA0] = T|5;                // MOV AL,mem
    inssize[0xA1] = T|5;                // MOV RAX,mem
    inssize[0xA2] = T|5;                // MOV mem,AL
    inssize[0xA3] = T|5;                // MOV mem,RAX
    BPRM = 5;                           // [RBP] addressing mode

    fregsaved = mBP | mBX | mR12 | mR13 | mR14 | mR15 | mES;      // saved across function calls
    FLOATREGS = FLOATREGS_64;
    FLOATREGS2 = FLOATREGS2_64;
    DOUBLEREGS = DOUBLEREGS_64;
    STACKALIGN = 16;

#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
    ALLREGS = mAX|mBX|mCX|mDX|mSI|mDI|  mR8|mR9|mR10|mR11|mR12|mR13|mR14|mR15;
    BYTEREGS = ALLREGS;
#endif

    for (unsigned i = 0x80; i < 0x90; i++)
        inssize2[i] = W|T|6;
}

/*********************************
 * Word or dword align start of function.
 */
void cod3_align_bytes(size_t nbytes)
{
    static unsigned char nops[] = {
        0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
    }; // XCHG AX,AX
    assert(nbytes < sizeof(nops));
    assert(SegData[cseg]->SDseg == cseg);
    obj_write_bytes(SegData[cseg],nbytes,nops);
}

void cod3_align()
{
    unsigned nbytes;
#if TARGET_WINDOS
    if (config.flags4 & CFG4speed)      // if optimized for speed
    {
        // Pick alignment based on CPU target
        if (config.target_cpu == TARGET_80486 ||
            config.target_cpu >= TARGET_PentiumPro)
        {   // 486 does reads on 16 byte boundaries, so if we are near
            // such a boundary, align us to it

            nbytes = -Coffset & 15;
            if (nbytes < 8)
                cod3_align_bytes(nbytes);
        }
    }
#else
    nbytes = -Coffset & 3;
    cod3_align_bytes(nbytes);
#endif
}

/*****************************
 * Given a type, return a mask of
 * registers to hold that type.
 * Input:
 *      tyf     function type
 */

regm_t regmask(tym_t tym, tym_t tyf)
{
    switch (tybasic(tym))
    {
        case TYvoid:
        case TYstruct:
            return 0;
        case TYbool:
        case TYwchar_t:
        case TYchar16:
        case TYchar:
        case TYschar:
        case TYuchar:
        case TYshort:
        case TYushort:
        case TYint:
        case TYuint:
#if JHANDLE
        case TYjhandle:
#endif
        case TYnullptr:
        case TYnptr:
#if TARGET_SEGMENTED
        case TYsptr:
        case TYcptr:
#endif
            return mAX;

        case TYfloat:
        case TYifloat:
            if (I64)
                return mXMM0;
            if (config.exe & EX_flat)
                return mST0;
        case TYlong:
        case TYulong:
        case TYdchar:
            if (!I16)
                return mAX;
#if TARGET_SEGMENTED
        case TYfptr:
        case TYhptr:
#endif
            return mDX | mAX;

        case TYcent:
        case TYucent:
            assert(I64);
            return mDX | mAX;

#if TARGET_SEGMENTED
        case TYvptr:
            return mDX | mBX;
#endif

        case TYdouble:
        case TYdouble_alias:
        case TYidouble:
            if (I64)
                return mXMM0;
            if (config.exe & EX_flat)
                return mST0;
            return DOUBLEREGS;

        case TYllong:
        case TYullong:
            return I64 ? mAX : (I32 ? mDX | mAX : DOUBLEREGS);

        case TYldouble:
        case TYildouble:
            return mST0;

        case TYcfloat:
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
            if (I32 && tybasic(tyf) == TYnfunc)
                return mDX | mAX;
#endif
        case TYcdouble:
            if (I64)
                return mXMM0 | mXMM1;
        case TYcldouble:
            return mST01;

        // SIMD vector types
        case TYfloat4:
        case TYdouble2:
        case TYschar16:
        case TYuchar16:
        case TYshort8:
        case TYushort8:
        case TYlong4:
        case TYulong4:
        case TYllong2:
        case TYullong2:
            if (!config.fpxmmregs)
            {   printf("SIMD operations not supported on this platform\n");
                exit(1);
            }
            return mXMM0;

        default:
#if DEBUG
            WRTYxx(tym);
#endif
            assert(0);
            return 0;
    }
}

/*******************************
 * setup register allocator parameters with platform specific data
 */
void cgreg_dst_regs(unsigned *dst_integer_reg, unsigned *dst_float_reg)
{
    *dst_integer_reg = AX;
    *dst_float_reg   = XMM0;
}

void cgreg_set_priorities(tym_t ty, char **pseq, char **pseqmsw)
{
    unsigned sz = tysize(ty);

    if (tyxmmreg(ty))
    {
        static char sequence[] = {XMM0,XMM1,XMM2,XMM3,XMM4,XMM5,XMM6,XMM7,NOREG};
        *pseq = sequence;
    }
    else if (I64)
    {
        if (sz == REGSIZE * 2)
        {
            static char seqmsw[] = {CX,DX,NOREG};
            static char seqlsw[] = {AX,BX,SI,DI,NOREG};
            *pseq = seqlsw;
            *pseqmsw = seqmsw;
        }
        else
        {   // R10 is reserved for the static link
            static char sequence[] = {AX,CX,DX,SI,DI,R8,R9,R11,BX,R12,R13,R14,R15,BP,NOREG};
            *pseq = sequence;
        }
    }
    else if (I32)
    {
        if (sz == REGSIZE * 2)
        {
            static char seqlsw[] = {AX,BX,SI,DI,NOREG};
            static char seqmsw[] = {CX,DX,NOREG};
            *pseq = seqlsw;
            *pseqmsw = seqmsw;
        }
        else
        {
            static char sequence[] = {AX,CX,DX,BX,SI,DI,BP,NOREG};
            *pseq = sequence;
        }
    }
    else
    {   assert(I16);
        if (typtr(ty))
        {
            // For pointer types, try to pick index register first
            static char seqidx[] = {BX,SI,DI,AX,CX,DX,BP,NOREG};
            *pseq = seqidx;
        }
        else
        {
            // Otherwise, try to pick index registers last
            static char sequence[] = {AX,CX,DX,BX,SI,DI,BP,NOREG};
            *pseq = sequence;
        }
    }
}


/*******************************
 * Generate block exit code
 */
void outblkexitcode(block *bl, code*& c, int& anyspill, const char* sflsave, symbol** retsym, const regm_t mfuncregsave)
{
    elem *e = bl->Belem;
    block *nextb;
    block *bs1,*bs2;
    regm_t retregs = 0;
    bool jcond;

    switch (bl->BC)                     /* block exit condition         */
    {
        case BCiftrue:
            jcond = TRUE;
            bs1 = list_block(bl->Bsucc);
            bs2 = list_block(list_next(bl->Bsucc));
            if (bs1 == bl->Bnext)
            {   // Swap bs1 and bs2
                block *btmp;

                jcond ^= 1;
                btmp = bs1;
                bs1 = bs2;
                bs2 = btmp;
            }
            c = cat(c,logexp(e,jcond,FLblock,(code *) bs1));
            nextb = bs2;
            bl->Bcode = NULL;
        L2:
            if (nextb != bl->Bnext)
            {   if (configv.addlinenumbers && bl->Bsrcpos.Slinnum &&
                    !(funcsym_p->ty() & mTYnaked))
                    cgen_linnum(&c,bl->Bsrcpos);
                assert(!(bl->Bflags & BFLepilog));
                c = cat(c,genjmp(CNIL,JMP,FLblock,nextb));
            }
            bl->Bcode = cat(bl->Bcode,c);
            break;
        case BCjmptab:
        case BCifthen:
        case BCswitch:
            assert(!(bl->Bflags & BFLepilog));
            doswitch(bl);               /* hide messy details           */
            bl->Bcode = cat(c,bl->Bcode);
            break;
#if MARS
        case BCjcatch:
            // Mark all registers as destroyed. This will prevent
            // register assignments to variables used in catch blocks.
            c = cat(c,getregs((I32 | I64) ? allregs : (ALLREGS | mES)));
#if 0 && TARGET_LINUX
            if (config.flags3 & CFG3pic && !(allregs & mBX))
            {
                c = cat(c, cod3_load_got());
            }
#endif
            goto case_goto;
#endif
#if SCPP
        case BCcatch:
            // Mark all registers as destroyed. This will prevent
            // register assignments to variables used in catch blocks.
            c = cat(c,getregs(allregs | mES));
#if 0 && TARGET_LINUX
            if (config.flags3 & CFG3pic && !(allregs & mBX))
            {
                c = cat(c, cod3_load_got());
            }
#endif
            goto case_goto;

        case BCtry:
            usednteh |= EHtry;
            if (config.flags2 & CFG2seh)
                usednteh |= NTEHtry;
            goto case_goto;
#endif
        case BCgoto:
            nextb = list_block(bl->Bsucc);
            if ((funcsym_p->Sfunc->Fflags3 & Fnteh ||
                 (MARS /*&& config.flags2 & CFG2seh*/)) &&
                bl->Btry != nextb->Btry &&
                nextb->BC != BC_finally)
            {   int toindex;
                int fromindex;

                bl->Bcode = NULL;
                c = gencodelem(c,e,&retregs,TRUE);
                toindex = nextb->Btry ? nextb->Btry->Bscope_index : -1;
                assert(bl->Btry);
                fromindex = bl->Btry->Bscope_index;
#if MARS
                if (toindex + 1 == fromindex)
                {   // Simply call __finally
                    if (bl->Btry &&
                        list_block(list_next(bl->Btry->Bsucc))->BC == BCjcatch)
                    {
                        goto L2;
                    }
                }
#endif
                if (config.flags2 & CFG2seh)
                    c = cat(c,nteh_unwind(0,toindex));
#if MARS && (TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS)
                else if (toindex + 1 <= fromindex)
                {
                    //c = cat(c, linux_unwind(0, toindex));
                    block *bt;

                    //printf("B%d: fromindex = %d, toindex = %d\n", bl->Bdfoidx, fromindex, toindex);
                    bt = bl;
                    while ((bt = bt->Btry) != NULL && bt->Bscope_index != toindex)
                    {   block *bf;

                        //printf("\tbt->Bscope_index = %d, bt->Blast_index = %d\n", bt->Bscope_index, bt->Blast_index);
                        bf = list_block(list_next(bt->Bsucc));
                        // Only look at try-finally blocks
                        if (bf->BC == BCjcatch)
                            continue;

                        if (bf == nextb)
                            continue;
                        //printf("\tbf = B%d, nextb = B%d\n", bf->Bdfoidx, nextb->Bdfoidx);
                        if (nextb->BC == BCgoto &&
                            !nextb->Belem &&
                            bf == list_block(nextb->Bsucc))
                            continue;

                        // call __finally
                        code *cs;
                        code *cr;
                        int nalign = 0;

                        gensaverestore(retregs,&cs,&cr);
                        if (STACKALIGN == 16)
                        {   int npush = (numbitsset(retregs) + 1) * REGSIZE;
                            if (npush & (STACKALIGN - 1))
                            {   nalign = STACKALIGN - (npush & (STACKALIGN - 1));
                                cs = genc2(cs,0x81,modregrm(3,5,SP),nalign); // SUB ESP,nalign
                                if (I64)
                                    code_orrex(cs, REX_W);
                            }
                        }
                        cs = genc(cs,0xE8,0,0,0,FLblock,(targ_size_t)list_block(bf->Bsucc));
                        if (nalign)
                        {   cs = genc2(cs,0x81,modregrm(3,0,SP),nalign); // ADD ESP,nalign
                            if (I64)
                                code_orrex(cs, REX_W);
                        }
                        c = cat3(c,cs,cr);
                    }
                }
#endif
                goto L2;
            }
        case_goto:
            c = gencodelem(c,e,&retregs,TRUE);
            if (anyspill)
            {   // Add in the epilog code
                code *cstore = NULL;
                code *cload = NULL;

                for (int i = 0; i < anyspill; i++)
                {   symbol *s = globsym.tab[i];

                    if (s->Sflags & SFLspill &&
                        vec_testbit(dfoidx,s->Srange))
                    {
                        s->Sfl = sflsave[i];    // undo block register assignments
                        cgreg_spillreg_epilog(bl,s,&cstore,&cload);
                    }
                }
                c = cat3(c,cstore,cload);
            }

        L3:
            bl->Bcode = NULL;
            nextb = list_block(bl->Bsucc);
            goto L2;

        case BC_try:
            if (config.flags2 & CFG2seh)
            {   usednteh |= NTEH_try;
                nteh_usevars();
            }
            else
                usednteh |= EHtry;
            goto case_goto;

        case BC_finally:
            // Mark all registers as destroyed. This will prevent
            // register assignments to variables used in finally blocks.
            assert(!getregs(allregs));
            assert(!e);
            assert(!bl->Bcode);
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
            if (config.flags3 & CFG3pic)
            {
                int nalign = 0;
                if (STACKALIGN == 16)
                {   nalign = STACKALIGN - REGSIZE;
                    c = genc2(c,0x81,modregrm(3,5,SP),nalign); // SUB ESP,nalign
                    if (I64)
                        code_orrex(c, REX_W);
                }
                // CALL bl->Bsucc
                c = genc(c,0xE8,0,0,0,FLblock,(targ_size_t)list_block(bl->Bsucc));
                if (nalign)
                {   c = genc2(c,0x81,modregrm(3,0,SP),nalign); // ADD ESP,nalign
                    if (I64)
                        code_orrex(c, REX_W);
                }
                // JMP list_next(bl->Bsucc)
                nextb = list_block(list_next(bl->Bsucc));
                goto L2;
            }
            else
#endif
            {
                // Generate a PUSH of the address of the successor to the
                // corresponding BC_ret
                //assert(list_block(list_next(bl->Bsucc))->BC == BC_ret);
                // PUSH &succ
                c = genc(c,0x68,0,0,0,FLblock,(targ_size_t)list_block(list_next(bl->Bsucc)));
                nextb = list_block(bl->Bsucc);
                goto L2;
            }

        case BC_ret:
            c = gencodelem(c,e,&retregs,TRUE);
            bl->Bcode = gen1(c,0xC3);   // RET
            break;

#if NTEXCEPTIONS
        case BC_except:
            assert(!e);
            usednteh |= NTEH_except;
            c = cat(c,nteh_setsp(0x8B));
            getregs(allregs);
            goto L3;

        case BC_filter:
            c = cat(c,nteh_filter(bl));
            // Mark all registers as destroyed. This will prevent
            // register assignments to variables used in filter blocks.
            getregs(allregs);
            retregs = regmask(e->Ety, TYnfunc);
            c = gencodelem(c,e,&retregs,TRUE);
            bl->Bcode = gen1(c,0xC3);   // RET
            break;
#endif

        case BCretexp:
            retregs = regmask(e->Ety, funcsym_p->ty());

            // For the final load into the return regs, don't set regcon.used,
            // so that the optimizer can potentially use retregs for register
            // variable assignments.

            if (config.flags4 & CFG4optimized)
            {   regm_t usedsave;

                c = cat(c,docommas(&e));
                usedsave = regcon.used;
                if (EOP(e))
                    c = gencodelem(c,e,&retregs,TRUE);
                else
                {
                    if (e->Eoper == OPconst)
                        regcon.mvar = 0;
                    c = gencodelem(c,e,&retregs,TRUE);
                    regcon.used = usedsave;
                    if (e->Eoper == OPvar)
                    {   symbol *s = e->EV.sp.Vsym;

                        if (s->Sfl == FLreg && s->Sregm != mAX)
                            *retsym = s;
                    }
                }
            }
            else
            {
        case BCret:
        case BCexit:
                c = gencodelem(c,e,&retregs,TRUE);
            }
            bl->Bcode = c;
            if (retregs == mST0)
            {   assert(stackused == 1);
                pop87();                // account for return value
            }
            else if (retregs == mST01)
            {   assert(stackused == 2);
                pop87();
                pop87();                // account for return value
            }
            if (bl->BC == BCexit && config.flags4 & CFG4optimized)
                mfuncreg = mfuncregsave;
            if (MARS || usednteh & NTEH_try)
            {   block *bt;

                bt = bl;
                while ((bt = bt->Btry) != NULL)
                {   block *bf;

                    bf = list_block(list_next(bt->Bsucc));
#if MARS
                    // Only look at try-finally blocks
                    if (bf->BC == BCjcatch)
                    {
                        continue;
                    }
#endif
                    if (config.flags2 & CFG2seh)
                    {
                        if (bt->Bscope_index == 0)
                        {
                            // call __finally
                            code *cs;
                            code *cr;

                            c = cat(c,nteh_gensindex(-1));
                            gensaverestore(retregs,&cs,&cr);
                            cs = genc(cs,0xE8,0,0,0,FLblock,(targ_size_t)list_block(bf->Bsucc));
                            bl->Bcode = cat3(c,cs,cr);
                        }
                        else
                            bl->Bcode = cat(c,nteh_unwind(retregs,~0));
                        break;
                    }
                    else
                    {
                        // call __finally
                        code *cs;
                        code *cr;
                        int nalign = 0;

                        gensaverestore(retregs,&cs,&cr);
                        if (STACKALIGN == 16)
                        {   int npush = (numbitsset(retregs) + 1) * REGSIZE;
                            if (npush & (STACKALIGN - 1))
                            {   nalign = STACKALIGN - (npush & (STACKALIGN - 1));
                                cs = genc2(cs,0x81,modregrm(3,5,SP),nalign); // SUB ESP,nalign
                                if (I64)
                                    code_orrex(cs, REX_W);
                            }
                        }
                        // CALL bf->Bsucc
                        cs = genc(cs,0xE8,0,0,0,FLblock,(targ_size_t)list_block(bf->Bsucc));
                        if (nalign)
                        {   cs = genc2(cs,0x81,modregrm(3,0,SP),nalign); // ADD ESP,nalign
                            if (I64)
                                code_orrex(cs, REX_W);
                        }
                        bl->Bcode = c = cat3(c,cs,cr);
                    }
                }
            }
            break;

#if SCPP || MARS
        case BCasm:
            assert(!e);
            // Mark destroyed registers
            assert(!c);
            c = cat(c,getregs(iasm_regs(bl)));
            if (bl->Bsucc)
            {   nextb = list_block(bl->Bsucc);
                if (!bl->Bnext)
                    goto L2;
                if (nextb != bl->Bnext &&
                    bl->Bnext &&
                    !(bl->Bnext->BC == BCgoto &&
                     !bl->Bnext->Belem &&
                     nextb == list_block(bl->Bnext->Bsucc)))
                {   code *cl;

                    // See if already have JMP at end of block
                    cl = code_last(bl->Bcode);
                    if (!cl || cl->Iop != JMP)
                        goto L2;        // add JMP at end of block
                }
            }
            break;
#endif
        default:
#ifdef DEBUG
            printf("bl->BC = %d\n",bl->BC);
#endif
            assert(0);
    }
}

/*******************************
 * Generate code for blocks ending in a switch statement.
 * Take BCswitch and decide on
 *      BCifthen        use if - then code
 *      BCjmptab        index into jump table
 *      BCswitch        search table for match
 */

void doswitch(block *b)
{   code *cc,*c,*ce;
    regm_t retregs;
    unsigned ncases,n,reg,reg2,rm;
    targ_llong vmax,vmin,val;
    targ_llong *p;
    list_t bl;
    elem *e;

    tym_t tys;
    int sz;
    unsigned char dword;
    unsigned char mswsame;
#if LONGLONG
    targ_ulong msw;
#else
    unsigned msw;
#endif

    e = b->Belem;
    elem_debug(e);
    cc = docommas(&e);
    cgstate.stackclean++;
    tys = tybasic(e->Ety);
    sz = tysize[tys];
    dword = (sz == 2 * REGSIZE);
    mswsame = 1;                        // assume all msw's are the same
    p = b->BS.Bswitch;                  /* pointer to case data         */
    assert(p);
    ncases = *p++;                      /* number of cases              */

    vmax = MINLL;                       // smallest possible llong
    vmin = MAXLL;                       // largest possible llong
    for (n = 0; n < ncases; n++)        // find max and min case values
    {   val = *p++;
        if (val > vmax) vmax = val;
        if (val < vmin) vmin = val;
        if (REGSIZE == 2)
        {
            unsigned short ms = (val >> 16) & 0xFFFF;
            if (n == 0)
                msw = ms;
            else if (msw != ms)
                mswsame = 0;
        }
        else // REGSIZE == 4
        {
            targ_ulong ms = (val >> 32) & 0xFFFFFFFF;
            if (n == 0)
                msw = ms;
            else if (msw != ms)
                mswsame = 0;
        }
    }
    p -= ncases;
    //dbg_printf("vmax = x%lx, vmin = x%lx, vmax-vmin = x%lx\n",vmax,vmin,vmax - vmin);

    if (I64)
    {   // For now, just generate basic if-then sequence to get us running
        retregs = ALLREGS;
        b->BC = BCifthen;
        c = scodelem(e,&retregs,0,TRUE);
        assert(!dword);                 // 128 bit switches not supported
        reg = findreg(retregs);         // reg that result is in
        bl = b->Bsucc;
        for (n = 0; n < ncases; n++)
        {   code *cx;
            val = *p;
            if (sz == 4)
                cx = genc2(CNIL,0x81,modregrmx(3,7,reg),val);  // CMP reg,val
            else if (sz == 8)
            {
                if (val == (int)val)    // if val is a 64 bit value sign-extended from 32 bits
                {
                    cx = genc2(CNIL,0x81,modregrmx(3,7,reg),val);  // CMP reg,value32
                    cx->Irex |= REX_W;  // 64 bit operand
                }
                else
                {   unsigned sreg;
                                                                // MOV sreg,value64
                    cx = regwithvalue(CNIL, ALLREGS & ~mask[reg], val, &sreg, 64);
                    cx = genregs(cx,0x3B,reg,sreg);             // CMP reg,sreg
                    code_orrex(cx, REX_W);
                }
            }
            else
                assert(0);
            bl = list_next(bl);
            genjmp(cx,JE,FLblock,list_block(bl));       // JE caseaddr
            c = cat(c,cx);
            p++;
        }
        if (list_block(b->Bsucc) != b->Bnext) /* if default is not next block */
                c = cat(c,genjmp(CNIL,JMP,FLblock,list_block(b->Bsucc)));
        ce = NULL;
    }
    // Need to do research on MACHOBJ to see about better methods
    else if (MACHOBJ || ncases <= 3)
    {   // generate if-then sequence
        retregs = ALLREGS;
    L1:
        b->BC = BCifthen;
        c = scodelem(e,&retregs,0,TRUE);
        if (dword)
        {   reg = findreglsw(retregs);
            reg2 = findregmsw(retregs);
        }
        else
            reg = findreg(retregs);     /* reg that result is in        */
        bl = b->Bsucc;
        if (dword && mswsame)
        {   /* CMP reg2,MSW     */
            c = genc2(c,0x81,modregrm(3,7,reg2),msw);
            genjmp(c,JNE,FLblock,list_block(b->Bsucc)); /* JNE default  */
        }
        for (n = 0; n < ncases; n++)
        {   code *cnext = CNIL;
                                        /* CMP reg,casevalue            */
            c = cat(c,ce = genc2(CNIL,0x81,modregrm(3,7,reg),(targ_int)*p));
            if (dword && !mswsame)
            {
                cnext = gennop(CNIL);
                genjmp(ce,JNE,FLcode,(block *) cnext);
                genc2(ce,0x81,modregrm(3,7,reg2),MSREG(*p));
            }
            bl = list_next(bl);
                                        /* JE caseaddr                  */
            genjmp(ce,JE,FLblock,list_block(bl));
            c = cat(c,cnext);
            p++;
        }
        if (list_block(b->Bsucc) != b->Bnext) /* if default is not next block */
                c = cat(c,genjmp(CNIL,JMP,FLblock,list_block(b->Bsucc)));
        ce = NULL;
    }
#if TARGET_WINDOS               // try and find relocation to support this
    else if ((targ_ullong)(vmax - vmin) <= ncases * 2)  // then use jump table
    {   int modify;

        b->BC = BCjmptab;
        retregs = IDXREGS;
        if (dword)
            retregs |= mMSW;
        modify = (vmin || !I32);
        c = scodelem(e,&retregs,0,!modify);
        reg = findreg(retregs & IDXREGS); /* reg that result is in      */
        if (dword)
            reg2 = findregmsw(retregs);
        if (modify)
        {
            assert(!(retregs & regcon.mvar));
            c = cat(c,getregs(retregs));
        }
        if (vmin)                       /* if there is a minimum        */
        {
            c = genc2(c,0x81,modregrm(3,5,reg),vmin); /* SUB reg,vmin   */
            if (dword)
            {   genc2(c,0x81,modregrm(3,3,reg2),MSREG(vmin)); // SBB reg2,vmin
                genjmp(c,JNE,FLblock,list_block(b->Bsucc)); /* JNE default  */
            }
        }
        else if (dword)
        {   c = gentstreg(c,reg2);              // TEST reg2,reg2
            genjmp(c,JNE,FLblock,list_block(b->Bsucc)); /* JNE default  */
        }
        if (vmax - vmin != REGMASK)     /* if there is a maximum        */
        {                               /* CMP reg,vmax-vmin            */
            c = genc2(c,0x81,modregrm(3,7,reg),vmax-vmin);
            genjmp(c,JA,FLblock,list_block(b->Bsucc));  /* JA default   */
        }
        if (!I32)
            c = gen2(c,0xD1,modregrm(3,4,reg)); /* SHL reg,1            */
        if (I32)
        {
            ce = genc1(CNIL,0xFF,modregrm(0,4,4),FLswitch,0); /* JMP [CS:]disp[idxreg*4] */
            ce->Isib = modregrm(2,reg,5);
        }
        else
        {   rm = getaddrmode(retregs) | modregrm(0,4,0);
            ce = genc1(CNIL,0xFF,rm,FLswitch,0);        /* JMP [CS:]disp[idxreg] */
        }
        int flags = (config.flags & CFGromable) ? CFcs : 0; // table is in code seg
        ce->Iflags |= flags;                    // segment override
        ce->IEV1.Vswitch = b;
        b->Btablesize = (int) (vmax - vmin + 1) * tysize[TYnptr];
    }
#endif
    else                                /* else use switch table (BCswitch) */
    {   targ_size_t disp;
        int mod;
        code *esw;
        code *ct;

        retregs = mAX;                  /* SCASW requires AX            */
        if (dword)
            retregs |= mDX;
        else if (ncases <= 6 || config.flags4 & CFG4speed)
            goto L1;
        c = scodelem(e,&retregs,0,TRUE);
        if (dword && mswsame)
        {   /* CMP DX,MSW       */
            c = genc2(c,0x81,modregrm(3,7,DX),msw);
            genjmp(c,JNE,FLblock,list_block(b->Bsucc)); /* JNE default  */
        }
        ce = getregs(mCX|mDI);
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
        if (config.flags3 & CFG3pic)
        {   // Add in GOT
            code *cx;
            code *cgot;

            ce = cat(ce, getregs(mDX));
            cx = genc2(NULL,CALL,0,0);  //     CALL L1
            gen1(cx, 0x58 + DI);        // L1: POP EDI

                                        //     ADD EDI,_GLOBAL_OFFSET_TABLE_+3
            symbol *gotsym = elfobj_getGOTsym();
            cgot = gencs(CNIL,0x81,modregrm(3,0,DI),FLextern,gotsym);
            cgot->Iflags = CFoff;
            cgot->IEVoffset2 = 3;

            makeitextern(gotsym);

            genmovreg(cgot, DX, DI);    // MOV EDX, EDI
                                        // ADD EDI,offset of switch table
            esw = gencs(CNIL,0x81,modregrm(3,0,DI),FLswitch,NULL);
            esw->IEV2.Vswitch = b;
            esw = cat3(cx, cgot, esw);
        }
        else
#endif
        {
                                        // MOV DI,offset of switch table
            esw = gencs(CNIL,0xC7,modregrm(3,0,DI),FLswitch,NULL);
            esw->IEV2.Vswitch = b;
        }
        ce = cat(ce,esw);
        movregconst(ce,CX,ncases,0);    /* MOV CX,ncases                */

        /* The switch table will be accessed through ES:DI.
         * Therefore, load ES with proper segment value.
         */
        if (config.flags3 & CFG3eseqds)
        {   assert(!(config.flags & CFGromable));
            ce = cat(ce,getregs(mCX));          // allocate CX
        }
        else
        {
            ce = cat(ce,getregs(mES|mCX));      // allocate ES and CX
            gen1(ce,(config.flags & CFGromable) ? 0x0E : 0x1E); // PUSH CS/DS
            gen1(ce,0x07);                      // POP  ES
        }

        disp = (ncases - 1) * intsize;          /* displacement to jump table */
        if (dword && !mswsame)
        {   code *cloop;

            /* Build the following:
                L1:     SCASW
                        JNE     L2
                        CMP     DX,[CS:]disp[DI]
                L2:     LOOPNE  L1
             */

            mod = (disp > 127) ? 2 : 1;         /* displacement size    */
            cloop = genc2(CNIL,0xE0,0,-7 - mod -
                ((config.flags & CFGromable) ? 1 : 0)); /* LOOPNE scasw */
            ce = gen1(ce,0xAF);                         /* SCASW        */
            code_orflag(ce,CFtarg2);                    // target of jump
            genjmp(ce,JNE,FLcode,(block *) cloop);      /* JNE loop     */
                                                /* CMP DX,[CS:]disp[DI] */
            ct = genc1(CNIL,0x39,modregrm(mod,DX,5),FLconst,disp);
            int flags = (config.flags & CFGromable) ? CFcs : 0; // table is in code seg
            ct->Iflags |= flags;                // possible seg override
            ce = cat3(ce,ct,cloop);
            disp += ncases * intsize;           /* skip over msw table  */
        }
        else
        {
            ce = gen1(ce,0xF2);         /* REPNE                        */
            gen1(ce,0xAF);              /* SCASW                        */
        }
        genjmp(ce,JNE,FLblock,list_block(b->Bsucc)); /* JNE default     */
        mod = (disp > 127) ? 2 : 1;     /* 1 or 2 byte displacement     */
        if (config.flags & CFGromable)
                gen1(ce,SEGCS);         /* table is in code segment     */
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
        if (config.flags3 & CFG3pic)
        {                               // ADD EDX,(ncases-1)*2[EDI]
            ct = genc1(CNIL,0x03,modregrm(mod,DX,7),FLconst,disp);
                                        // JMP EDX
            gen2(ct,0xFF,modregrm(3,4,DX));
        }
        else
#endif
        {                               // JMP (ncases-1)*2[DI]
            ct = genc1(CNIL,0xFF,modregrm(mod,4,(I32 ? 7 : 5)),FLconst,disp);
            int flags = (config.flags & CFGromable) ? CFcs : 0; // table is in code seg
            ct->Iflags |= flags;
        }
        ce = cat(ce,ct);
        b->Btablesize = disp + intsize + ncases * tysize[TYnptr];
    }
    b->Bcode = cat3(cc,c,ce);
    //assert(b->Bcode);
    cgstate.stackclean--;
}

/******************************
 * Output data block for a jump table (BCjmptab).
 * The 'holes' in the table get filled with the
 * default label.
 */

void outjmptab(block *b)
{
  unsigned ncases,n;
  targ_llong u,vmin,vmax,val,*p;
  targ_size_t alignbytes,def,targ,*poffset;
  int jmpseg;

  poffset = (config.flags & CFGromable) ? &Coffset : &JMPOFF;
  p = b->BS.Bswitch;                    /* pointer to case data         */
  ncases = *p++;                        /* number of cases              */
  vmax = MINLL;                 // smallest possible llong
  vmin = MAXLL;                 // largest possible llong
  for (n = 0; n < ncases; n++)          /* find min case value          */
  {     val = p[n];
        if (val > vmax) vmax = val;
        if (val < vmin) vmin = val;
  }
  jmpseg = (config.flags & CFGromable) ? cseg : JMPSEG;

  /* Any alignment bytes necessary */
  alignbytes = align(0,*poffset) - *poffset;
  obj_lidata(jmpseg,*poffset,alignbytes);

  def = list_block(b->Bsucc)->Boffset;  /* default address              */
  assert(vmin <= vmax);
  for (u = vmin; ; u++)
  {     targ = def;                     /* default                      */
        for (n = 0; n < ncases; n++)
        {       if (p[n] == u)
                {       targ = list_block(list_nth(b->Bsucc,n + 1))->Boffset;
                        break;
                }
        }
        reftocodseg(jmpseg,*poffset,targ);
        *poffset += tysize[TYnptr];
        if (u == vmax)                  /* for case that (vmax == ~0)   */
                break;
  }
}

/******************************
 * Output data block for a switch table.
 * Two consecutive tables, the first is the case value table, the
 * second is the address table.
 */

void outswitab(block *b)
{ unsigned ncases,n;
  targ_llong *p;
  targ_size_t val;
  targ_size_t alignbytes,*poffset;
  int seg;                              /* target segment for table     */
  list_t bl;
  unsigned sz;
  targ_size_t offset;

  //printf("outswitab()\n");
  p = b->BS.Bswitch;                    /* pointer to case data         */
  ncases = *p++;                        /* number of cases              */

  if (config.flags & CFGromable)
  {     poffset = &Coffset;
        assert(cseg == CODE);
        seg = cseg;
  }
  else
  {
        poffset = &JMPOFF;
        seg = JMPSEG;
  }
  offset = *poffset;
  alignbytes = align(0,*poffset) - *poffset;
  obj_lidata(seg,*poffset,alignbytes);  /* any alignment bytes necessary */
  assert(*poffset == offset + alignbytes);

  sz = intsize;
  assert(SegData[seg]->SDseg == seg);
  for (n = 0; n < ncases; n++)          /* send out value table         */
  {
        //printf("\tcase %d, offset = x%x\n", n, *poffset);
        obj_write_bytes(SegData[seg],sz,p);
        p++;
  }
  offset += alignbytes + sz * ncases;
  assert(*poffset == offset);

  if (b->Btablesize == ncases * (REGSIZE * 2 + tysize[TYnptr]))
  {
        /* Send out MSW table   */
        p -= ncases;
        for (n = 0; n < ncases; n++)
        {   val = MSREG(*p);
            p++;
            obj_write_bytes(SegData[seg],REGSIZE,&val);
        }
        offset += REGSIZE * ncases;
        assert(*poffset == offset);
  }

  bl = b->Bsucc;
  for (n = 0; n < ncases; n++)          /* send out address table       */
  {     bl = list_next(bl);
        reftocodseg(seg,*poffset,list_block(bl)->Boffset);
        *poffset += tysize[TYnptr];
  }
  assert(*poffset == offset + ncases * tysize[TYnptr]);
}

/*****************************
 * Return a jump opcode relevant to the elem for a JMP TRUE.
 */

int jmpopcode(elem *e)
{ tym_t tym;
  int zero,i,jp,op;
  static const char jops[][2][6] =
    {   /* <=  >   <   >=  ==  !=    <=0 >0  <0  >=0 ==0 !=0    */
       { {JLE,JG ,JL ,JGE,JE ,JNE},{JLE,JG ,JS ,JNS,JE ,JNE} }, /* signed   */
       { {JBE,JA ,JB ,JAE,JE ,JNE},{JE ,JNE,JB ,JAE,JE ,JNE} }, /* unsigned */
#if 0
       { {JLE,JG ,JL ,JGE,JE ,JNE},{JLE,JG ,JL ,JGE,JE ,JNE} }, /* real     */
       { {JBE,JA ,JB ,JAE,JE ,JNE},{JBE,JA ,JB ,JAE,JE ,JNE} }, /* 8087     */
       { {JA ,JBE,JAE,JB ,JE ,JNE},{JBE,JA ,JB ,JAE,JE ,JNE} }, /* 8087 R   */
#endif
    };

#define XP      (JP  << 8)
#define XNP     (JNP << 8)
    static const unsigned jfops[1][26] =
    /*   le     gt lt     ge  eqeq    ne     unord lg  leg  ule ul uge  */
    {
      { XNP|JBE,JA,XNP|JB,JAE,XNP|JE, XP|JNE,JP,   JNE,JNP, JBE,JC,XP|JAE,

    /*  ug    ue ngt nge nlt    nle    ord nlg nleg nule nul nuge    nug     nue */
        XP|JA,JE,JBE,JB, XP|JAE,XP|JA, JNP,JE, JP,  JA,  JNC,XNP|JB, XNP|JBE,JNE        }, /* 8087     */
    };

  assert(e);
  while (e->Eoper == OPcomma ||
        /* The !EOP(e->E1) is to line up with the case in cdeq() where  */
        /* we decide if mPSW is passed on when evaluating E2 or not.    */
         (e->Eoper == OPeq && !EOP(e->E1)))
        e = e->E2;                      /* right operand determines it  */

  op = e->Eoper;
  if (e->Ecount != e->Ecomsub)          // comsubs just get Z bit set
        return JNE;
  if (!OTrel(op))                       // not relational operator
  {
        tym_t tymx = tybasic(e->Ety);
        if (tyfloating(tymx) && config.inline8087 &&
            (tymx == TYldouble || tymx == TYildouble || tymx == TYcldouble ||
             tymx == TYcdouble || tymx == TYcfloat ||
             op == OPind))
        {
            return XP|JNE;
        }
        return (op >= OPbt && op <= OPbts) ? JC : JNE;
  }

  if (e->E2->Eoper == OPconst)
        zero = !boolres(e->E2);
  else
        zero = 0;

  tym = e->E1->Ety;
  if (tyfloating(tym))
#if 1
  {     i = 0;
        if (config.inline8087)
        {   i = 1;

#if 1
#define NOSAHF (I64 || config.fpxmmregs)
            if (rel_exception(op) || config.flags4 & CFG4fastfloat)
            {
                if (zero)
                {
                    if (NOSAHF)
                        op = swaprel(op);
                }
                else if (NOSAHF)
                    op = swaprel(op);
                else if (cmporder87(e->E2))
                    op = swaprel(op);
                else
                    ;
            }
            else
            {
                if (zero && config.target_cpu < TARGET_80386)
                    ;
                else
                    op = swaprel(op);
            }
#else
            if (zero && !rel_exception(op) && config.target_cpu >= TARGET_80386)
                op = swaprel(op);
            else if (!zero &&
                (cmporder87(e->E2) || !(rel_exception(op) || config.flags4 & CFG4fastfloat)))
                /* compare is reversed */
                op = swaprel(op);
#endif
        }
        jp = jfops[0][op - OPle];
        goto L1;
  }
#else
        i = (config.inline8087) ? (3 + cmporder87(e->E2)) : 2;
#endif
  else if (tyuns(tym) || tyuns(e->E2->Ety))
        i = 1;
  else if (tyintegral(tym) || typtr(tym))
        i = 0;
  else
  {
#if DEBUG
        elem_print(e);
        WRTYxx(tym);
#endif
        assert(0);
  }

  jp = jops[i][zero][op - OPle];        /* table starts with OPle       */
L1:
#if DEBUG
  if ((jp & 0xF0) != 0x70)
        WROP(op),
        printf("i %d zero %d op x%x jp x%x\n",i,zero,op,jp);
#endif
  assert((jp & 0xF0) == 0x70);
  return jp;
}

/**********************************
 * Append code to *pc which validates pointer described by
 * addressing mode in *pcs. Modify addressing mode in *pcs.
 * Input:
 *      keepmsk mask of registers we must not destroy or use
 *              if (keepmsk & RMstore), this will be only a store operation
 *              into the lvalue
 */

void cod3_ptrchk(code **pc,code *pcs,regm_t keepmsk)
{   code *c;
    code *cs2;
    unsigned char rm,sib;
    unsigned reg;
    unsigned flagsave;
    unsigned opsave;
    regm_t idxregs;
    regm_t tosave;
    regm_t used;
    int i;

    assert(!I64);
    if (!I16 && pcs->Iflags & (CFes | CFss | CFcs | CFds | CFfs | CFgs))
        return;         // not designed to deal with 48 bit far pointers

    c = *pc;

    rm = pcs->Irm;
    assert(!(rm & 0x40));       // no disp8 or reg addressing modes

    // If the addressing mode is already a register
    reg = rm & 7;
    if (I16)
    {   static const unsigned char imode[8] = { BP,BP,BP,BP,SI,DI,BP,BX };

        reg = imode[reg];               // convert [SI] to SI, etc.
    }
    idxregs = mask[reg];
    if ((rm & 0x80 && (pcs->IFL1 != FLoffset || pcs->IEV1.Vuns)) ||
        !(idxregs & ALLREGS)
       )
    {
        // Load the offset into a register, so we can push the address
        idxregs = (I16 ? IDXREGS : ALLREGS) & ~keepmsk; // only these can be index regs
        assert(idxregs);
        c = cat(c,allocreg(&idxregs,&reg,TYoffset));

        opsave = pcs->Iop;
        flagsave = pcs->Iflags;
        pcs->Iop = 0x8D;
        pcs->Irm |= modregrm(0,reg,0);
        pcs->Iflags &= ~(CFopsize | CFss | CFes | CFcs);        // no prefix bytes needed
        c = gen(c,pcs);                 // LEA reg,EA

        pcs->Iflags = flagsave;
        pcs->Iop = opsave;
    }

    // registers destroyed by the function call
    //used = (mBP | ALLREGS | mES) & ~fregsaved;
    used = 0;                           // much less code generated this way

    cs2 = CNIL;
    tosave = used & (keepmsk | idxregs);
    for (i = 0; tosave; i++)
    {   regm_t mi = mask[i];

        assert(i < REGMAX);
        if (mi & tosave)        /* i = register to save                 */
        {
            int push,pop;

            stackchanged = 1;
            if (i == ES)
            {   push = 0x06;
                pop = 0x07;
            }
            else
            {   push = 0x50 + i;
                pop = push | 8;
            }
            c = gen1(c,push);                   // PUSH i
            cs2 = cat(gen1(CNIL,pop),cs2);      // POP i
            tosave &= ~mi;
        }
    }

    // For 16 bit models, push a far pointer
    if (I16)
    {   int segreg;

        switch (pcs->Iflags & (CFes | CFss | CFcs | CFds | CFfs | CFgs))
        {   case CFes:  segreg = 0x06;  break;
            case CFss:  segreg = 0x16;  break;
            case CFcs:  segreg = 0x0E;  break;
            case 0:     segreg = 0x1E;  break;  // DS
            default:
                assert(0);
        }

        // See if we should default to SS:
        // (Happens when BP is part of the addressing mode)
        if (segreg == 0x1E && (rm & 0xC0) != 0xC0 &&
            rm & 2 && (rm & 7) != 7)
        {   segreg = 0x16;
            if (config.wflags & WFssneds)
                pcs->Iflags |= CFss;    // because BP won't be there anymore
        }
        c = gen1(c,segreg);             // PUSH segreg
    }

    c = gen1(c,0x50 + reg);             // PUSH reg

    // Rewrite the addressing mode in *pcs so it is just 0[reg]
    setaddrmode(pcs, idxregs);
    pcs->IFL1 = FLoffset;
    pcs->IEV1.Vuns = 0;

    // Call the validation function
    {
        makeitextern(rtlsym[RTLSYM_PTRCHK]);

        used &= ~(keepmsk | idxregs);           // regs destroyed by this exercise
        c = cat(c,getregs(used));
                                                // CALL __ptrchk
        gencs(c,(LARGECODE) ? 0x9A : CALL,0,FLfunc,rtlsym[RTLSYM_PTRCHK]);
    }

    *pc = cat(c,cs2);
}



/***********************************
 * Determine if BP can be used as a general purpose register.
 * Note parallels between this routine and prolog().
 * Returns:
 *      0       can't be used, needed for frame
 *      mBP     can be used
 */

regm_t cod3_useBP()
{
    tym_t tym;
    tym_t tyf;

    // Note that DOSX memory model cannot use EBP as a general purpose
    // register, as SS != DS.
    if (!(config.exe & EX_flat) || config.flags & (CFGalwaysframe | CFGnoebp))
        goto Lcant;

    if (anyiasm)
        goto Lcant;

    tyf = funcsym_p->ty();
    if (tyf & mTYnaked)                 // if no prolog/epilog for function
        goto Lcant;

    if (funcsym_p->Sfunc->Fflags3 & Ffakeeh)
    {
        goto Lcant;                     // need consistent stack frame
    }

    tym = tybasic(tyf);
    if (tym == TYifunc)
        goto Lcant;

    stackoffsets(0);
    localsize = Aoffset;                // an estimate only
//    if (localsize)
    {
        if (!(config.flags4 & CFG4speed) ||
            config.target_cpu < TARGET_Pentium ||
            tyfarfunc(tym) ||
            config.flags & CFGstack ||
            localsize >= 0x100 ||       // arbitrary value < 0x1000
            (usednteh & ~NTEHjmonitor) ||
            usedalloca
           )
            goto Lcant;
    }
Lcan:
    return mBP;

Lcant:
    return 0;
}

/***************************************
 * Gen code for OPframeptr
 */

code *cdframeptr(elem *e, regm_t *pretregs)
{
    unsigned reg;
    code cs;

    regm_t retregs = *pretregs & allregs;
    if  (!retregs)
        retregs = allregs;
    code *cg = allocreg(&retregs, &reg, TYint);

    cs.Iop = ESCAPE | ESCframeptr;
    cs.Iflags = 0;
    cs.Irex = 0;
    cs.Irm = reg;
    cg = gen(cg,&cs);

    return cat(cg,fixresult(e,retregs,pretregs));
}

/***************************************
 * Gen code for load of _GLOBAL_OFFSET_TABLE_.
 * This value gets cached in the local variable 'localgot'.
 */

code *cdgot(elem *e, regm_t *pretregs)
{
#if TARGET_OSX
    regm_t retregs;
    unsigned reg;
    code *c;

    retregs = *pretregs & allregs;
    if  (!retregs)
        retregs = allregs;
    c = allocreg(&retregs, &reg, TYnptr);

    c = genc(c,CALL,0,0,0,FLgot,0);     //     CALL L1
    gen1(c, 0x58 + reg);                // L1: POP reg

    return cat(c,fixresult(e,retregs,pretregs));
#elif TARGET_LINUX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
    regm_t retregs;
    unsigned reg;
    code *c;
    code *cgot;

    retregs = *pretregs & allregs;
    if  (!retregs)
        retregs = allregs;
    c = allocreg(&retregs, &reg, TYnptr);

    c = genc2(c,CALL,0,0);      //     CALL L1
    gen1(c, 0x58 + reg);        // L1: POP reg

                                //     ADD reg,_GLOBAL_OFFSET_TABLE_+3
    symbol *gotsym = elfobj_getGOTsym();
    cgot = gencs(CNIL,0x81,modregrm(3,0,reg),FLextern,gotsym);
    /* Because the 2:3 offset from L1: is hardcoded,
     * this sequence of instructions must not
     * have any instructions in between,
     * so set CFvolatile to prevent the scheduler from rearranging it.
     */
    cgot->Iflags = CFoff | CFvolatile;
    cgot->IEVoffset2 = (reg == AX) ? 2 : 3;

    makeitextern(gotsym);
    return cat3(c,cgot,fixresult(e,retregs,pretregs));
#else
    assert(0);
    return NULL;
#endif
}

/**************************************************
 * Load contents of localgot into EBX.
 */

code *load_localgot()
{
#if TARGET_LINUX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
    if (config.flags3 & CFG3pic && I32)
    {
        if (localgot)
        {
            localgot->Sflags &= ~GTregcand;     // because this hack doesn't work with reg allocator
            elem *e = el_var(localgot);
            regm_t retregs = mBX;
            code *c = codelem(e,&retregs,FALSE);
            el_free(e);
            return c;
        }
        else
        {
            elem *e = el_long(TYnptr, 0);
            e->Eoper = OPgot;
            regm_t retregs = mBX;
            code *c = codelem(e,&retregs,FALSE);
            el_free(e);
            return c;
        }
    }
#endif
    return NULL;
}

#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
/*****************************
 * Returns:
 *      # of bytes stored
 */

#define ONS_OHD 4               // max # of extra bytes added by obj_namestring()

STATIC int obj_namestring(char *p,const char *name)
{   unsigned len;

    len = strlen(name);
    if (len > 255)
    {
        short *ps = (short *)p;
        p[0] = 0xFF;
        p[1] = 0;
        ps[1] = len;
        memcpy(p + 4,name,len);
        len += ONS_OHD;
    }
    else
    {   p[0] = len;
        memcpy(p + 1,name,len);
        len++;
    }
    return len;
}
#endif

code *genregs(code *c,unsigned op,unsigned dstreg,unsigned srcreg)
{ return gen2(c,op,modregxrmx(3,dstreg,srcreg)); }

code *gentstreg(code *c,unsigned t)
{
    c = gen2(c,0x85,modregxrmx(3,t,t));   // TEST t,t
    code_orflag(c,CFpsw);
    return c;
}

code *genpush(code *c, unsigned reg)
{
    c = gen1(c, 0x50 + (reg & 7));
    if (reg & 8)
        code_orrex(c, REX_B);
    return c;
}

code *genpop(code *c, unsigned reg)
{
    c = gen1(c, 0x58 + (reg & 7));
    if (reg & 8)
        code_orrex(c, REX_B);
    return c;
}

/**************************
 * Generate a MOV to save a register to a stack slot
 */
code *gensavereg(unsigned& reg, targ_uns slot)
{
    // MOV i[BP],reg
    unsigned op = 0x89;              // normal mov
    if (reg == ES)
    {   reg = 0;            // the real reg number
        op = 0x8C;          // segment reg mov
    }
    code *c = genc1(NULL,op,modregxrm(2, reg, BPRM),FLcs,slot);
    if (I64)
        code_orrex(c, REX_W);

    return c;
}

/**************************
 * Generate a MOV to,from register instruction.
 * Smart enough to dump redundant register moves, and segment
 * register moves.
 */

code *genmovreg(code *c,unsigned to,unsigned from)
{
#if DEBUG
        if (to > ES || from > ES)
                printf("genmovreg(c = %p, to = %d, from = %d)\n",c,to,from);
#endif
        assert(to <= ES && from <= ES);
        if (to != from)
        {
                if (to == ES)
                        c = genregs(c,0x8E,0,from);
                else if (from == ES)
                        c = genregs(c,0x8C,0,to);
                else
                        c = genregs(c,0x89,from,to);
                if (I64)
                        code_orrex(c, REX_W);
        }
        return c;
}

/***************************************
 * Generate immediate multiply instruction for r1=r2*imm.
 * Optimize it into LEA's if we can.
 */

code *genmulimm(code *c,unsigned r1,unsigned r2,targ_int imm)
{   code cs;

    // These optimizations should probably be put into pinholeopt()
    switch (imm)
    {   case 1:
            c = genmovreg(c,r1,r2);
            break;
        case 5:
            cs.Iop = LEA;
            cs.Iflags = 0;
            cs.Irex = 0;
            buildEA(&cs,r2,r2,4,0);
            cs.orReg(r1);
            c = gen(c,&cs);
            break;
        default:
            c = genc2(c,0x69,modregxrmx(3,r1,r2),imm);    // IMUL r1,r2,imm
            break;
    }
    return c;
}

/******************************
 * Load CX with the value of _AHSHIFT.
 */

code *genshift(code *c)
{
#if SCPP && TX86
    code *c1;

    // Set up ahshift to trick ourselves into giving the right fixup,
    // which must be seg-relative, external frame, external target.
    c1 = gencs(CNIL,0xC7,modregrm(3,0,CX),FLfunc,rtlsym[RTLSYM_AHSHIFT]);
    c1->Iflags |= CFoff;
    return cat(c,c1);
#else
    assert(0);
    return 0;
#endif
}

/******************************
 * Move constant value into reg.
 * Take advantage of existing values in registers.
 * If flags & mPSW
 *      set flags based on result
 * Else if flags & 8
 *      do not disturb flags
 * Else
 *      don't care about flags
 * If flags & 1 then byte move
 * If flags & 2 then short move (for I32 and I64)
 * If flags & 4 then don't disturb unused portion of register
 * If flags & 16 then reg is a byte register AL..BH
 * If flags & 64 (0x40) then 64 bit move (I64 only)
 * Returns:
 *      code (if any) generated
 */

code *movregconst(code *c,unsigned reg,targ_size_t value,regm_t flags)
{   unsigned r;
    regm_t mreg;

    //printf("movregconst(reg=%s, value= %lld (%llx), flags=%x)\n", regm_str(mask[reg]), value, value, flags);
#define genclrreg(a,r) genregs(a,0x31,r,r)

    regm_t regm = regcon.immed.mval & mask[reg];
    targ_size_t regv = regcon.immed.value[reg];

    if (flags & 1)      // 8 bits
    {
        value &= 0xFF;
        regm &= BYTEREGS;

        // If we already have the right value in the right register
        if (regm && (regv & 0xFF) == value)
            goto L2;

        if (flags & 16 && reg & 4 &&    // if an H byte register
            regcon.immed.mval & mask[reg & 3] &&
            (((regv = regcon.immed.value[reg & 3]) >> 8) & 0xFF) == value)
            goto L2;

        /* Avoid byte register loads on Pentium Pro and Pentium II
         * to avoid dependency stalls.
         */
        if (config.flags4 & CFG4speed &&
            config.target_cpu >= TARGET_PentiumPro && !(flags & 4))
            goto L3;

        // See if another register has the right value
        r = 0;
        for (mreg = (regcon.immed.mval & BYTEREGS); mreg; mreg >>= 1)
        {
            if (mreg & 1)
            {
                if ((regcon.immed.value[r] & 0xFF) == value)
                {   c = genregs(c,0x8A,reg,r);          // MOV regL,rL
                    if (I64 && reg >= 4 || r >= 4)
                        code_orrex(c, REX);
                    goto L2;
                }
                if (!(I64 && reg >= 4) &&
                    r < 4 && ((regcon.immed.value[r] >> 8) & 0xFF) == value)
                {   c = genregs(c,0x8A,reg,r | 4);      // MOV regL,rH
                    goto L2;
                }
            }
            r++;
        }

        if (value == 0 && !(flags & 8))
        {
            if (!(flags & 4) &&                 // if we can set the whole register
                !(flags & 16 && reg & 4))       // and reg is not an H register
            {   c = genregs(c,0x31,reg,reg);    // XOR reg,reg
                regimmed_set(reg,value);
                regv = 0;
            }
            else
                c = genregs(c,0x30,reg,reg);    // XOR regL,regL
            flags &= ~mPSW;                     // flags already set by XOR
        }
        else
        {   c = genc2(c,0xC6,modregrmx(3,0,reg),value);  /* MOV regL,value */
            if (reg >= 4 && I64)
            {
                code_orrex(c, REX);
            }
        }
    L2:
        if (flags & mPSW)
            genregs(c,0x84,reg,reg);            // TEST regL,regL

        if (regm)
            // Set just the 'L' part of the register value
            regimmed_set(reg,(regv & ~(targ_size_t)0xFF) | value);
        else if (flags & 16 && reg & 4 && regcon.immed.mval & mask[reg & 3])
            // Set just the 'H' part of the register value
            regimmed_set((reg & 3),(regv & ~(targ_size_t)0xFF00) | (value << 8));
        return c;
    }
L3:
    if (I16)
        value = (targ_short) value;             /* sign-extend MSW      */
    else if (I32)
        value = (targ_int) value;

    if (!I16 && flags & 2)                      // load 16 bit value
    {
        value &= 0xFFFF;
        if (value == 0)
            goto L1;
        else
        {
            if (flags & mPSW)
                goto L1;
            code *c1 = genc2(CNIL,0xC7,modregrmx(3,0,reg),value); // MOV reg,value
            c1->Iflags |= CFopsize;             // yes, even for I64
            c = cat(c,c1);
            if (regm)
                // High bits of register are not affected by 16 bit load
                regimmed_set(reg,(regv & ~(targ_size_t)0xFFFF) | value);
        }
        return c;
    }
L1:

    /* If we already have the right value in the right register */
    if (regm && (regv & 0xFFFFFFFF) == (value & 0xFFFFFFFF) && !(flags & 64))
    {   if (flags & mPSW)
            c = gentstreg(c,reg);
    }
    else if (flags & 64 && regm && regv == value)
    {   // Look at the full 64 bits
        if (flags & mPSW)
        {
            c = gentstreg(c,reg);
            code_orrex(c, REX_W);
        }
    }
    else
    {
        if (flags & mPSW)
        {
            switch (value)
            {   case 0:
                    c = genclrreg(c,reg);
                    if (flags & 64)
                        code_orrex(c, REX_W);
                    break;
                case 1:
                    if (I64)
                        goto L4;
                    c = genclrreg(c,reg);
                    goto inc;
                case -1:
                    if (I64)
                        goto L4;
                    c = genclrreg(c,reg);
                    goto dec;
                default:
                L4:
                    if (flags & 64)
                    {
                        c = genc2(c,0xC7,(REX_W << 16) | modregrmx(3,0,reg),value); // MOV reg,value64
                        gentstreg(c,reg);
                        code_orrex(c, REX_W);
                    }
                    else
                    {   c = genc2(c,0xC7,modregrmx(3,0,reg),value); /* MOV reg,value */
                        gentstreg(c,reg);
                    }
                    break;
            }
        }
        else
        {
            /* Look for single byte conversion  */
            if (regcon.immed.mval & mAX)
            {
                if (I32)
                {   if (reg == AX && value == (targ_short) regv)
                    {   c = gen1(c,0x98);               /* CWDE         */
                        goto done;
                    }
                    if (reg == DX &&
                        value == (regcon.immed.value[AX] & 0x80000000 ? 0xFFFFFFFF : 0) &&
                        !(config.flags4 & CFG4speed && config.target_cpu >= TARGET_Pentium)
                       )
                    {   c = gen1(c,0x99);               /* CDQ          */
                        goto done;
                    }
                }
                else if (I16)
                {
                    if (reg == AX &&
                        (targ_short) value == (signed char) regv)
                    {   c = gen1(c,0x98);               /* CBW          */
                        goto done;
                    }

                    if (reg == DX &&
                        (targ_short) value == (regcon.immed.value[AX] & 0x8000 ? (targ_short) 0xFFFF : (targ_short) 0) &&
                        !(config.flags4 & CFG4speed && config.target_cpu >= TARGET_Pentium)
                       )
                    {   c = gen1(c,0x99);               /* CWD          */
                        goto done;
                    }
                }
            }
            if (value == 0 && !(flags & 8) && config.target_cpu >= TARGET_80486)
            {   c = genclrreg(c,reg);           // CLR reg
                if (flags & 64)
                    code_orrex(c, REX_W);
                goto done;
            }

            if (!I64 && regm && !(flags & 8))
            {   if (regv + 1 == value ||
                    /* Catch case of (0xFFFF+1 == 0) for 16 bit compiles */
                    (I16 && (targ_short)(regv + 1) == (targ_short)value))
                {
                inc:
                    c = gen1(c,0x40 + reg);     /* INC reg              */
                    goto done;
                }
                if (regv - 1 == value)
                {
                dec:
                    c = gen1(c,0x48 + reg);     /* DEC reg              */
                    goto done;
                }
            }

            /* See if another register has the right value      */
            r = 0;
            for (mreg = regcon.immed.mval; mreg; mreg >>= 1)
            {
#ifdef DEBUG
                assert(!I16 || regcon.immed.value[r] == (targ_short)regcon.immed.value[r]);
#endif
                if (mreg & 1 && regcon.immed.value[r] == value)
                {   c = genmovreg(c,reg,r);
                    if (flags & 64)
                        code_orrex(c, REX_W);
                    goto done;
                }
                r++;
            }

            if (value == 0 && !(flags & 8))
            {   c = genclrreg(c,reg);           // CLR reg
                if (flags & 64)
                    code_orrex(c, REX_W);
            }
            else
            {   /* See if we can just load a byte       */
                if (regm & BYTEREGS &&
                    !(config.flags4 & CFG4speed && config.target_cpu >= TARGET_PentiumPro)
                   )
                {
                    if ((regv & ~(targ_size_t)0xFF) == (value & ~(targ_size_t)0xFF))
                    {   c = movregconst(c,reg,value,(flags & 8) |4|1);  // load regL
                        return c;
                    }
                    if (regm & (mAX|mBX|mCX|mDX) &&
                        (regv & ~(targ_size_t)0xFF00) == (value & ~(targ_size_t)0xFF00) &&
                        !I64)
                    {   c = movregconst(c,4|reg,value >> 8,(flags & 8) |4|1|16); // load regH
                        return c;
                    }
                }
                if (flags & 64)
                    c = genc2(c,0xC7,(REX_W << 16) | modregrmx(3,0,reg),value); // MOV reg,value64
                else
                    c = genc2(c,0xC7,modregrmx(3,0,reg),value); // MOV reg,value
            }
        }
    done:
        regimmed_set(reg,value);
    }
    return c;
}

/**************************
 * Generate a jump instruction.
 */

code *genjmp(code *c,unsigned op,unsigned fltarg,block *targ)
{   code cs;
    code *cj;
    code *cnop;

    cs.Iop = op & 0xFF;
    cs.Iflags = 0;
    cs.Irex = 0;
    if (op != JMP && op != 0xE8)        // if not already long branch
          cs.Iflags = CFjmp16;          /* assume long branch for op = 0x7x */
    cs.IFL2 = fltarg;                   /* FLblock (or FLcode)          */
    cs.IEV2.Vblock = targ;              /* target block (or code)       */
    if (fltarg == FLcode)
        ((code *)targ)->Iflags |= CFtarg;

    if (config.flags4 & CFG4fastfloat)  // if fast floating point
        return gen(c,&cs);

    cj = gen(CNIL,&cs);
    switch (op & 0xFF00)                /* look at second jump opcode   */
    {
        /* The JP and JNP come from floating point comparisons          */
        case JP << 8:
            cs.Iop = JP;
            gen(cj,&cs);
            break;
        case JNP << 8:
            /* Do a JP around the jump instruction      */
            cnop = gennop(CNIL);
            c = genjmp(c,JP,FLcode,(block *) cnop);
            cat(cj,cnop);
            break;
        case 1 << 8:                    /* toggled no jump              */
        case 0 << 8:
            break;
        default:
#ifdef DEBUG
            printf("jop = x%x\n",op);
#endif
            assert(0);
    }
    return cat(c,cj);
}

/*******************************
 * Generate code for a function start.
 * Input:
 *      Coffset         address of start of code
 * Output:
 *      Coffset         adjusted for size of code generated
 *      EBPtoESP
 *      hasframe
 *      BPoff
 */

code *prolog()
{
    SYMIDX si;
    unsigned reg;
    char enter;
    unsigned Foffset;
    unsigned xlocalsize;     // amount to subtract from ESP to make room for locals
    unsigned pushallocreg;
    char guessneedframe;
    regm_t namedargs = 0;

    //printf("cod3.prolog(), needframe = %d, Aalign = %d\n", needframe, Aalign);
    debugx(debugw && printf("funcstart()\n"));
    regcon.immed.mval = 0;                      /* no values in registers yet   */
    EBPtoESP = -REGSIZE;
    hasframe = 0;
    char pushds = 0;
    BPoff = 0;
    code *c = CNIL;
    int pushalloc = 0;
    tym_t tyf = funcsym_p->ty();
    tym_t tym = tybasic(tyf);
    unsigned farfunc = tyfarfunc(tym);
    pushallocreg = (tyf == TYmfunc) ? CX : AX;
    if (config.flags & CFGalwaysframe || funcsym_p->Sfunc->Fflags3 & Ffakeeh)
        needframe = 1;

Lagain:
    guessneedframe = needframe;
//    if (needframe && config.exe & (EX_LINUX | EX_FREEBSD | EX_SOLARIS) && !(usednteh & ~NTEHjmonitor))
//      usednteh |= NTEHpassthru;

    /* Compute BP offsets for variables on stack.
     * The organization is:
     *  Poff    parameters
     *          seg of return addr      (if far function)
     *          IP of return addr
     *  BP->    caller's BP
     *          DS                      (if Windows prolog/epilog)
     *          exception handling context symbol
     *  Aoff    autos and regs
     *  regsave.off  any saved registers
     *  Foff    floating register
     *  AAoff   alloca temporary
     *  CSoff   common subs
     *  NDPoff  any 8087 saved registers
     *  Toff    temporaries
     *          monitor context record
     *          any saved registers
     */

    if (tym == TYifunc)
        Poff = 26;
    else if (I64)
        Poff = 16;
    else if (I32)
        Poff = farfunc ? 12 : 8;
    else
        Poff = farfunc ? 6 : 4;

    Aoff = 0;
#if NTEXCEPTIONS == 2
    Aoff -= nteh_contextsym_size();
#if MARS
    if (funcsym_p->Sfunc->Fflags3 & Ffakeeh && nteh_contextsym_size() == 0)
        Aoff -= 5 * 4;
#endif
#endif
    Aoff = -align(0,-Aoff + Aoffset);

    regsave.off = Aoff - align(0,regsave.top);
    Foffset = floatreg ? (config.fpxmmregs ? 16 : DOUBLESIZE) : 0;
    Foff = regsave.off - align(0,Foffset);
    assert(usedalloca != 1);
    AAoff = usedalloca ? (Foff - REGSIZE) : Foff;
    CSoff = AAoff - align(0,cstop * REGSIZE);
    NDPoff = CSoff - align(0,NDP::savetop * NDPSAVESIZE);
    Toff = NDPoff - align(0,Toffset);

    if (Foffset > Aalign)
        Aalign = Foffset;
    if (Aalign > REGSIZE)
    {
        // Adjust Aoff so that it is Aalign byte aligned, assuming that
        // before function parameters were pushed the stack was
        // Aalign byte aligned
        targ_size_t psize = (Poffset + (REGSIZE - 1)) & ~(REGSIZE - 1);
        int sz = psize + -Aoff + Poff + (needframe ? 0 : REGSIZE);
        if (sz & (Aalign - 1))
        {   int adj = Aalign - (sz & (Aalign - 1));
            Aoff -= adj;
            regsave.off -= adj;
            Foff -= adj;
            AAoff -= adj;
            CSoff -= adj;
            NDPoff -= adj;
            Toff -= adj;
        }
    }

    localsize = -Toff;

    regm_t topush = fregsaved & ~mfuncreg;     // mask of registers that need saving
    int npush = 0;                      // number of registers that need saving
    for (regm_t x = topush; x; x >>= 1)
        npush += x & 1;

    // Keep the stack aligned by 8 for any subsequent function calls
    if (!I16 && calledafunc &&
        (STACKALIGN == 16 || config.flags4 & CFG4stackalign))
    {
        //printf("npush = %d Poff = x%x needframe = %d localsize = x%x\n", npush, Poff, needframe, localsize);

        int sz = Poff + (needframe ? 0 : -REGSIZE) + localsize + npush * REGSIZE;
        if (STACKALIGN == 16)
        {
            if (sz & (8|4))
                localsize += STACKALIGN - (sz & (8|4));
        }
        else if (sz & 4)
            localsize += 4;
    }

    //printf("Foff x%02x Aoff x%02x Toff x%02x NDPoff x%02x CSoff x%02x Poff x%02x localsize x%02x\n",
        //(int)Foff,(int)Aoff,(int)Toff,(int)NDPoff,(int)CSoff,(int)Poff,(int)localsize);

    xlocalsize = localsize;

    if (tyf & mTYnaked)                 // if no prolog/epilog for function
    {
        hasframe = 1;
        return NULL;
    }

    if (tym == TYifunc)
    {   static unsigned char ops2[] = { 0x60,0x1E,0x06,0 };
        static unsigned char ops0[] = { 0x50,0x51,0x52,0x53,
                                        0x54,0x55,0x56,0x57,
                                        0x1E,0x06,0 };

        unsigned char *p;

        p = (config.target_cpu >= TARGET_80286) ? ops2 : ops0;
        do
            c = gen1(c,*p);
        while (*++p);
        c = genregs(c,0x8B,BP,SP);                              // MOV BP,SP
        if (localsize)
            c = genc2(c,0x81,modregrm(3,5,SP),localsize);       // SUB SP,localsize
        tyf |= mTYloadds;
        hasframe = 1;
        goto Lcont;
    }

    /* Determine if we need BP set up   */
    if (config.flags & CFGalwaysframe)
        needframe = 1;
    else
    {
        if (localsize)
        {
            if (I16 ||
                !(config.flags4 & CFG4speed) ||
                config.target_cpu < TARGET_Pentium ||
                farfunc ||
                config.flags & CFGstack ||
                xlocalsize >= 0x1000 ||
                (usednteh & ~NTEHjmonitor) ||
                anyiasm ||
                usedalloca
               )
                needframe = 1;
        }
        if (refparam && (anyiasm || I16))
            needframe = 1;
    }

    if (needframe)
    {   assert(mfuncreg & mBP);         // shouldn't have used mBP

        if (!guessneedframe)            // if guessed wrong
            goto Lagain;
    }

    if (I16 && config.wflags & WFwindows && farfunc)
    {   int wflags;
        int segreg;

#if SCPP
        // alloca() can't be because the 'special' parameter won't be at
        // a known offset from BP.
        if (usedalloca == 1)
            synerr(EM_alloca_win);      // alloca() can't be in Windows functions
#endif

        wflags = config.wflags;
        if (wflags & WFreduced && !(tyf & mTYexport))
        {   // reduced prolog/epilog for non-exported functions
            wflags &= ~(WFdgroup | WFds | WFss);
        }

        c = getregs(mAX);
        assert(!c);                     /* should not have any value in AX */

        switch (wflags & (WFdgroup | WFds | WFss))
        {   case WFdgroup:                      // MOV  AX,DGROUP
                if (wflags & WFreduced)
                    tyf &= ~mTYloadds;          // remove redundancy
                c = genc(c,0xC7,modregrm(3,0,AX),0,0,FLdatseg,(targ_uns) 0);
                c->Iflags ^= CFseg | CFoff;     // turn off CFoff, on CFseg
                break;
            case WFss:
                segreg = 2;                     // SS
                goto Lmovax;
            case WFds:
                segreg = 3;                     // DS
            Lmovax:
                c = gen2(c,0x8C,modregrm(3,segreg,AX)); // MOV AX,segreg
                if (wflags & WFds)
                    gen1(c,0x90);               // NOP
                break;
            case 0:
                break;
            default:
#ifdef DEBUG
                printf("config.wflags = x%x\n",config.wflags);
#endif
                assert(0);
        }
        if (wflags & WFincbp)
            c = gen1(c,0x40 + BP);              // INC  BP
        c = gen1(c,0x50 + BP);                  // PUSH BP
        genregs(c,0x8B,BP,SP);                  // MOV  BP,SP
        if (wflags & (WFsaveds | WFds | WFss | WFdgroup))
        {   gen1(c,0x1E);                       // PUSH DS
            pushds = TRUE;
            BPoff = -REGSIZE;
        }
        if (wflags & (WFds | WFss | WFdgroup))
            gen2(c,0x8E,modregrm(3,3,AX));      // MOV  DS,AX

        enter = FALSE;                  /* don't use ENTER instruction  */
        hasframe = 1;                   /* we have a stack frame        */
    }
    else
    if (needframe)                      // if variables or parameters
    {
        if (config.wflags & WFincbp && farfunc)
            c = gen1(c,0x40 + BP);      /* INC  BP                      */
        if (config.target_cpu < TARGET_80286 ||
            config.exe & (EX_LINUX | EX_LINUX64 | EX_OSX | EX_OSX64 | EX_FREEBSD | EX_FREEBSD64 | EX_SOLARIS | EX_SOLARIS64) ||
            !localsize ||
            config.flags & CFGstack ||
            (xlocalsize >= 0x1000 && config.exe & EX_flat) ||
            localsize >= 0x10000 ||
#if NTEXCEPTIONS == 2
            (usednteh & ~NTEHjmonitor && (config.flags2 & CFG2seh)) ||
#endif
            (config.target_cpu >= TARGET_80386 &&
             config.flags4 & CFG4speed)
           )
        {
            c = gen1(c,0x50 + BP);      // PUSH BP
            genregs(c,0x8B,BP,SP);      // MOV  BP,SP
            if (I64)
                code_orrex(c, REX_W);   // MOV RBP,RSP
#if ELFOBJ || MACHOBJ
            if (config.fulltypes)
                // Don't reorder instructions, as dwarf CFA relies on it
                code_orflag(c, CFvolatile);
#endif
            enter = FALSE;              /* do not use ENTER instruction */
#if NTEXCEPTIONS == 2
            if (usednteh & ~NTEHjmonitor && (config.flags2 & CFG2seh))
            {
                code *ce = nteh_prolog();
                c = cat(c,ce);
                int sz = nteh_contextsym_size();
                assert(sz != 0);        // should be 5*4, not 0
                xlocalsize -= sz;       // sz is already subtracted from ESP
                                        // by nteh_prolog()
            }
#endif
#if ELFOBJ || MACHOBJ
            if (config.fulltypes)
            {   int off = I64 ? 16 : 8;
                dwarf_CFA_set_loc(1);           // address after PUSH EBP
                dwarf_CFA_set_reg_offset(SP, off); // CFA is now 8[ESP]
                dwarf_CFA_offset(BP, -off);       // EBP is at 0[ESP]
                dwarf_CFA_set_loc(3);           // address after MOV EBP,ESP
                // Yes, I know the parameter is 8 when we mean 0!
                // But this gets the cfa register set to EBP correctly
                dwarf_CFA_set_reg_offset(BP, off);        // CFA is now 0[EBP]
            }
#endif
        }
        else
            enter = TRUE;
        hasframe = 1;
    }

    if (config.flags & CFGstack)        /* if stack overflow check      */
        goto Ladjstack;

    if (needframe)                      /* if variables or parameters   */
    {
        if (xlocalsize)                 /* if any stack offset          */
        {
        Ladjstack:
#if !TARGET_LINUX               // seems that Linux doesn't need to fault in stack pages
            if ((config.flags & CFGstack && !(I32 && xlocalsize < 0x1000)) // if stack overflow check
#if TARGET_WINDOS
                || (xlocalsize >= 0x1000 && config.exe & EX_flat)
#endif
               )
            {
                if (I16)
                {
                    // BUG: Won't work if parameter is passed in AX
                    c = movregconst(c,AX,xlocalsize,FALSE); // MOV AX,localsize
                    makeitextern(rtlsym[RTLSYM_CHKSTK]);
                                                            // CALL _chkstk
                    gencs(c,(LARGECODE) ? 0x9A : CALL,0,FLfunc,rtlsym[RTLSYM_CHKSTK]);
                    useregs((ALLREGS | mBP | mES) & ~rtlsym[RTLSYM_CHKSTK]->Sregsaved);
                }
                else
                {
                    /* Watch out for 64 bit code where EDX is passed as a register parameter
                     */
                    int reg = I64 ? R11 : DX;  // scratch register

                    /*      MOV     EDX, xlocalsize/0x1000
                     *  L1: SUB     ESP, 0x1000
                     *      TEST    [ESP],ESP
                     *      DEC     EDX
                     *      JNE     L1
                     *      SUB     ESP, xlocalsize % 0x1000
                     */
                    c = movregconst(c, reg, xlocalsize / 0x1000, FALSE);
                    code *csub = genc2(NULL,0x81,modregrm(3,5,SP),0x1000);
                    if (I64)
                        code_orrex(csub, REX_W);
                    code_orflag(csub, CFtarg2);
                    gen2sib(csub, 0x85, modregrm(0,SP,4),modregrm(0,4,SP));
                    if (I64)
                    {   gen2(csub, 0xFF, (REX_W << 16) | modregrmx(3,0,R11));   // DEC R11
                        genc2(csub,JNE,0,(targ_uns)-14);
                    }
                    else
                    {   gen1(csub, 0x48 + DX);                  // DEC EDX
                        genc2(csub,JNE,0,(targ_uns)-12);
                    }
                    regimmed_set(reg,0);             // reg is now 0
                    genc2(csub,0x81,modregrm(3,5,SP),xlocalsize & 0xFFF);
                    if (I64)
                        code_orrex(csub, REX_W);
                    c = cat(c,csub);
                    useregs(mask[reg]);
                }
            }
            else
#endif
            {
                if (enter)
                {   // ENTER xlocalsize,0
                    c = genc(c,0xC8,0,FLconst,xlocalsize,FLconst,(targ_uns) 0);
#if ELFOBJ || MACHOBJ
                    assert(!config.fulltypes);          // didn't emit Dwarf data
#endif
                }
                else if (xlocalsize == REGSIZE && config.flags4 & CFG4optimized)
                {   c = gen1(c,0x50 + pushallocreg);    // PUSH AX
                    // Do this to prevent an -x[EBP] to be moved in
                    // front of the push.
                    code_orflag(c,CFvolatile);
                    pushalloc = 1;
                }
                else
                {   // SUB SP,xlocalsize
                    c = genc2(c,0x81,modregrm(3,5,SP),xlocalsize);
                    if (I64)
                        code_orrex(c, REX_W);
                }
            }

            if (usedalloca)
            {
                // Set up magic parameter for alloca()
                // MOV -REGSIZE[BP],localsize - BPoff
                //c = genc(c,0xC7,modregrm(2,0,BPRM),FLconst,-REGSIZE,FLconst,localsize - BPoff);
                c = genc(c,0xC7,modregrm(2,0,BPRM),
                        FLconst,AAoff + BPoff,
                        FLconst,localsize - BPoff);
                if (I64)
                    code_orrex(c, REX_W);
            }
        }
        else
            assert(usedalloca == 0);
    }
    else if (xlocalsize)
    {
        assert(I32);

        if (xlocalsize == REGSIZE)
        {   c = gen1(c,0x50 + pushallocreg);    // PUSH AX
            pushalloc = 1;
        }
        else if (xlocalsize == 2 * REGSIZE)
        {   c = gen1(c,0x50 + pushallocreg);    // PUSH AX
            gen1(c,0x50 + pushallocreg);        // PUSH AX
            pushalloc = 1;
        }
        else
        {   // SUB ESP,xlocalsize
            c = genc2(c,0x81,modregrm(3,5,SP),xlocalsize);
            if (I64)
                code_orrex(c, REX_W);
        }
        BPoff += REGSIZE;
    }
    else
        assert((localsize | usedalloca) == 0 || (usednteh & NTEHjmonitor));
    EBPtoESP += xlocalsize;

    /*  The idea is to generate trace for all functions if -Nc is not thrown.
     *  If -Nc is thrown, generate trace only for global COMDATs, because those
     *  are relevant to the FUNCTIONS statement in the linker .DEF file.
     *  This same logic should be in epilog().
     */
    if (config.flags & CFGtrace &&
        (!(config.flags4 & CFG4allcomdat) ||
         funcsym_p->Sclass == SCcomdat ||
         funcsym_p->Sclass == SCglobal ||
         (config.flags2 & CFG2comdat && SymInline(funcsym_p))
        )
       )
    {
        if (STACKALIGN == 16 && npush)
        {   /* This could be avoided by moving the function call to after the
             * registers are saved. But I don't remember why the call is here
             * and not there.
             */
            c = genc2(c,0x81,modregrm(3,5,SP),npush * REGSIZE); // SUB ESP,npush * REGSIZE
            if (I64)
                code_orrex(c, REX_W);
        }

        symbol *s = rtlsym[farfunc ? RTLSYM_TRACE_PRO_F : RTLSYM_TRACE_PRO_N];
        makeitextern(s);
        c = gencs(c,I16 ? 0x9A : CALL,0,FLfunc,s);      // CALL _trace
        if (!I16)
            code_orflag(c,CFoff | CFselfrel);
        /* Embedding the function name inline after the call works, but it
         * makes disassembling the code annoying.
         */
#if ELFOBJ || MACHOBJ
        size_t len = strlen(funcsym_p->Sident);
        char *buffer = (char *)malloc(len + 4);
        assert(buffer);
        if (len <= 254)
        {   buffer[0] = len;
            memcpy(buffer + 1, funcsym_p->Sident, len);
            len++;
        }
        else
        {   buffer[0] = 0xFF;
            buffer[1] = 0;
            buffer[2] = len & 0xFF;
            buffer[3] = len >> 8;
            memcpy(buffer + 4, funcsym_p->Sident, len);
            len += 4;
        }
        genasm(c, buffer, len);         // append func name
        free(buffer);
#else
        char name[IDMAX+IDOHD+1];
        size_t len = obj_mangle(funcsym_p,name);
        assert(len < sizeof(name));
        genasm(c,name,len);                             // append func name
#endif
        if (STACKALIGN == 16 && npush)
        {
            c = genc2(c,0x81,modregrm(3,0,SP),npush * REGSIZE); // ADD ESP,npush * REGSIZE
            if (I64)
                code_orrex(c, REX_W);
        }
        useregs((ALLREGS | mBP | mES) & ~s->Sregsaved);
    }

#if MARS
    if (usednteh & NTEHjmonitor)
    {   Symbol *sthis;

        for (si = 0; 1; si++)
        {   assert(si < globsym.top);
            sthis = globsym.tab[si];
            if (strcmp(sthis->Sident,"this") == 0)
                break;
        }
        c = cat(c,nteh_monitor_prolog(sthis));
        EBPtoESP += 3 * 4;
    }
#endif

    while (topush)                      /* while registers to push      */
    {   reg = findreg(topush);
        topush &= ~mask[reg];
        c = gen1(c,0x50 + (reg & 7));
        if (reg & 8)
            code_orrex(c, REX_B);
        EBPtoESP += REGSIZE;
#if ELFOBJ || MACHOBJ
        if (config.fulltypes)
        {   // Emit debug_frame data giving location of saved register
            // relative to 0[EBP]
            pinholeopt(c, NULL);
            dwarf_CFA_set_loc(calcblksize(c));  // address after PUSH reg
            dwarf_CFA_offset(reg, -EBPtoESP - REGSIZE);
        }
#endif
    }

Lcont:

    /* Determine if we need to reload DS        */
    if (tyf & mTYloadds)
    {   code *c1;

        if (!pushds)                            // if not already pushed
            c = gen1(c,0x1E);                   // PUSH DS
        c1 = genc(CNIL,0xC7,modregrm(3,0,AX),0,0,FLdatseg,(targ_uns) 0); /* MOV  AX,DGROUP      */
        c1->Iflags ^= CFseg | CFoff;            /* turn off CFoff, on CFseg */
        c = cat(c,c1);
        gen2(c,0x8E,modregrm(3,3,AX));            /* MOV  DS,AX         */
        useregs(mAX);
    }

    if (tym == TYifunc)
        c = gen1(c,0xFC);                       // CLD

#if NTEXCEPTIONS == 2
    if (usednteh & NTEH_except)
        c = cat(c,nteh_setsp(0x89));            // MOV __context[EBP].esp,ESP
#endif

    // Load register parameters off of the stack. Do not use
    // assignaddr(), as it will replace the stack reference with
    // the register!
    for (si = 0; si < globsym.top; si++)
    {   symbol *s = globsym.tab[si];
        code *c2;
        unsigned sz = type_size(s->Stype);

        if ((s->Sclass == SCregpar || s->Sclass == SCparameter) &&
            s->Sfl == FLreg &&
            (refparam
#if MARS
                // This variable has been reference by a nested function
                || s->Stype->Tty & mTYvolatile
#endif
                ))
        {
            /* MOV reg,param[BP]        */
            //assert(refparam);
            if (mask[s->Sreglsw] & XMMREGS)
            {
                unsigned op = xmmload(s->Stype->Tty);  // MOVSS/D xreg,mem
                unsigned xreg = s->Sreglsw - XMM0;
                code *c2 = genc1(CNIL,op,modregxrm(2,xreg,BPRM),FLconst,Poff + s->Soffset);
                if (!hasframe)
                {   // Convert to ESP relative address rather than EBP
                    c2->Irm = modregxrm(2,xreg,4);
                    c2->Isib = modregrm(0,4,SP);
                    c2->IEVpointer1 += EBPtoESP;
                }
                c = cat(c,c2);
            }
            else
            {
                code *c2 = genc1(CNIL,0x8B ^ (sz == 1),
                    modregxrm(2,s->Sreglsw,BPRM),FLconst,Poff + s->Soffset);
                if (!I16 && sz == SHORTSIZE)
                    c2->Iflags |= CFopsize; // operand size
                if (I64 && sz >= REGSIZE)
                    c2->Irex |= REX_W;
                if (!hasframe)
                {   /* Convert to ESP relative address rather than EBP      */
                    assert(!I16);
                    c2->Irm = modregxrm(2,s->Sreglsw,4);
                    c2->Isib = modregrm(0,4,SP);
                    c2->IEVpointer1 += EBPtoESP;
                }
                if (sz > REGSIZE)
                {
                    code *c3 = genc1(CNIL,0x8B,
                        modregxrm(2,s->Sregmsw,BPRM),FLconst,Poff + s->Soffset + REGSIZE);
                    if (I64)
                        c3->Irex |= REX_W;
                    if (!hasframe)
                    {   /* Convert to ESP relative address rather than EBP  */
                        assert(!I16);
                        c3->Irm = modregxrm(2,s->Sregmsw,4);
                        c3->Isib = modregrm(0,4,SP);
                        c3->IEVpointer1 += EBPtoESP;
                    }
                    c2 = cat(c2,c3);
                }
                c = cat(c,c2);
            }
        }
        else if (s->Sclass == SCfastpar)
        {   // Argument is passed in a register
            unsigned preg = s->Spreg;

            namedargs |= mask[preg];

            if (s->Sfl == FLreg)
            {   // MOV reg,preg
                if (mask[preg] & XMMREGS)
                {
                    unsigned op = xmmload(s->Stype->Tty);      // MOVSS/D xreg,preg
                    unsigned xreg = s->Sreglsw - XMM0;
                    c = gen2(c,op,modregxrmx(3,xreg,preg - XMM0));
                }
                else
                {
                    c = genmovreg(c,s->Sreglsw,preg);
                    if (I64 && sz == 8)
                        code_orrex(c, REX_W);
                }
            }
            else if (s->Sflags & SFLdead ||
                (!anyiasm && !(s->Sflags & SFLread) && s->Sflags & SFLunambig &&
#if MARS
                 // This variable has been reference by a nested function
                 !(s->Stype->Tty & mTYvolatile) &&
#endif
                 (config.flags4 & CFG4optimized || !config.fulltypes)))
            {
                // Ignore it, as it is never referenced
                ;
            }
            else
            {
                targ_size_t offset = Aoff + BPoff + s->Soffset;
                int op = 0x89;                  // MOV x[EBP],preg
                if (preg >= XMM0 && preg <= XMM15)
                {
                    op = xmmstore(s->Stype->Tty);
                }
                if (hasframe)
                {
                    if (!(pushalloc && preg == pushallocreg))
                    {
                        // MOV x[EBP],preg
                        c2 = genc1(CNIL,op,
                            modregxrm(2,preg,BPRM),FLconst, offset);
                        if (preg >= XMM0 && preg <= XMM15)
                        {
                        }
                        else
                        {
//printf("%s Aoff = %d, BPoff = %d, Soffset = %d, sz = %d\n", s->Sident, (int)Aoff, (int)BPoff, (int)s->Soffset, (int)sz);
//                          if (offset & 2)
//                              c2->Iflags |= CFopsize;
                            if (I64 && sz == 8)
                                code_orrex(c2, REX_W);
                        }
                        c = cat(c, c2);
                    }
                }
                else
                {
                    offset += EBPtoESP;
                    if (!(pushalloc && preg == pushallocreg))
                    {
                        // MOV offset[ESP],preg
                        // BUG: byte size?
                        c2 = genc1(CNIL,op,
                            (modregrm(0,4,SP) << 8) |
                            modregxrm(2,preg,4),FLconst,offset);
                        if (preg >= XMM0 && preg <= XMM15)
                        {
                        }
                        else
                        {
                            if (I64 && sz == 8)
                                c2->Irex |= REX_W;
//                          if (offset & 2)
//                              c2->Iflags |= CFopsize;
                        }
                        c = cat(c,c2);
                    }
                }
            }
        }
    }

    /* Load arguments passed in registers into the varargs save area
     * so they can be accessed by va_arg().
     */
    if (I64 && variadic(funcsym_p->Stype))
    {
        /* Look for __va_argsave
         */
        symbol *sv = NULL;
        for (SYMIDX si = 0; si < globsym.top; si++)
        {   symbol *s = globsym.tab[si];
            if (s->Sident[0] == '_' && strcmp(s->Sident, "__va_argsave") == 0)
            {   sv = s;
                break;
            }
        }

        if (sv && !(sv->Sflags & SFLdead))
        {
        /* Generate code to move any arguments passed in registers into
         * the stack variable __va_argsave,
         * so we can reference it via pointers through va_arg().
         *   struct __va_argsave_t {
         *     size_t[6] regs;
         *     real[8] fpregs;
         *     uint offset_regs;
         *     uint offset_fpregs;
         *     void* stack_args;
         *     void* reg_args;
         *   }
         * The MOVAPS instructions seg fault if data is not aligned on
         * 16 bytes, so this gives us a nice check to ensure no mistakes.
            MOV     voff+0*8[RBP],EDI
            MOV     voff+1*8[RBP],ESI
            MOV     voff+2*8[RBP],RDX
            MOV     voff+3*8[RBP],RCX
            MOV     voff+4*8[RBP],R8
            MOV     voff+5*8[RBP],R9
            MOVZX   EAX,AL                      // AL = 0..8, # of XMM registers used
            SHL     EAX,2                       // 4 bytes for each MOVAPS
            LEA     RDX,offset L2[RIP]
            SUB     RDX,RAX
            LEA     RAX,voff+6*8+0x7F[RBP]
            JMP     EDX
            MOVAPS  -0x0F[RAX],XMM7             // only save XMM registers if actually used
            MOVAPS  -0x1F[RAX],XMM6
            MOVAPS  -0x2F[RAX],XMM5
            MOVAPS  -0x3F[RAX],XMM4
            MOVAPS  -0x4F[RAX],XMM3
            MOVAPS  -0x5F[RAX],XMM2
            MOVAPS  -0x6F[RAX],XMM1
            MOVAPS  -0x7F[RAX],XMM0
          L2:
            MOV     1[RAX],offset_regs          // set __va_argsave.offset_regs
            MOV     5[RAX],offset_fpregs        // set __va_argsave.offset_fpregs
            LEA     RDX, Poff+Poffset[RBP]
            MOV     9[RAX],RDX                  // set __va_argsave.stack_args
            SUB     RAX,6*8+0x7F                // point to start of __va_argsave
            MOV     6*8+8*16+4+4+8[RAX],RAX     // set __va_argsave.reg_args
        */
        targ_size_t voff = Aoff + BPoff + sv->Soffset;  // EBP offset of start of sv
        const int vregnum = 6;
        const unsigned vsize = vregnum * 8 + 8 * 16;
        code *cv = CNIL;

        static unsigned char regs[vregnum] = { DI,SI,DX,CX,R8,R9 };

        if (!hasframe)
            voff += EBPtoESP;
        for (int i = 0; i < vregnum; i++)
        {
            unsigned r = regs[i];
            if (!(mask[r] & namedargs))         // named args are already dealt with
            {   unsigned ea = (REX_W << 16) | modregxrm(2,r,BPRM);
                if (!hasframe)
                    ea = (REX_W << 16) | (modregrm(0,4,SP) << 8) | modregxrm(2,r,4);
                cv = genc1(cv,0x89,ea,FLconst,voff + i*8);
            }
        }

        cv = genregs(cv,0x0FB6,AX,AX);                          // MOVZX EAX,AL
        genc2(cv,0xC1,modregrm(3,4,AX),2);                      // SHL EAX,2
        int raxoff = voff+6*8+0x7F;
        unsigned L2offset = (raxoff < -0x7F) ? 0x2C : 0x29;
        if (!hasframe)
            L2offset += 1;                                      // +1 for sib byte
        // LEA RDX,offset L2[RIP]
        genc1(cv,0x8D,(REX_W << 16) | modregrm(0,DX,5),FLconst,L2offset);
        genregs(cv,0x29,AX,DX);                                 // SUB RDX,RAX
        code_orrex(cv, REX_W);
        // LEA RAX,voff+vsize-6*8-16+0x7F[RBP]
        unsigned ea = (REX_W << 16) | modregrm(2,AX,BPRM);
        if (!hasframe)
            // add sib byte for [RSP] addressing
            ea = (REX_W << 16) | (modregrm(0,4,SP) << 8) | modregxrm(2,AX,4);
        genc1(cv,0x8D,ea,FLconst,raxoff);
        gen2(cv,0xFF,modregrm(3,4,DX));                         // JMP EDX
        for (int i = 0; i < 8; i++)
        {
            // MOVAPS -15-16*i[RAX],XMM7-i
            genc1(cv,0x0F29,modregrm(0,XMM7-i,0),FLconst,-15-16*i);
        }

        /* Compute offset_regs and offset_fpregs
         */
        unsigned offset_regs = 0;
        unsigned offset_fpregs = vregnum * 8;
        for (int i = AX; i <= XMM7; i++)
        {   regm_t m = mask[i];
            if (m & namedargs)
            {
                if (m & (mDI|mSI|mDX|mCX|mR8|mR9))
                    offset_regs += 8;
                else if (m & XMMREGS)
                    offset_fpregs += 16;
                namedargs &= ~m;
                if (!namedargs)
                    break;
            }
        }
        // MOV 1[RAX],offset_regs
        genc(cv,0xC7,modregrm(2,0,AX),FLconst,1,FLconst,offset_regs);

        // MOV 5[RAX],offset_fpregs
        genc(cv,0xC7,modregrm(2,0,AX),FLconst,5,FLconst,offset_fpregs);

        // LEA RDX, Poff+Poffset[RBP]
        ea = modregrm(2,DX,BPRM);
        if (!hasframe)
            ea = (modregrm(0,4,SP) << 8) | modregrm(2,DX,4);
        Poffset = (Poffset + (REGSIZE - 1)) & ~(REGSIZE - 1);
        genc1(cv,0x8D,(REX_W << 16) | ea,FLconst,Poff + Poffset);

        // MOV 9[RAX],RDX
        genc1(cv,0x89,(REX_W << 16) | modregrm(2,DX,AX),FLconst,9);

        // SUB RAX,6*8+0x7F             // point to start of __va_argsave
        genc2(cv,0x2D,0,6*8+0x7F);
        code_orrex(cv, REX_W);

        // MOV 6*8+8*16+4+4+8[RAX],RAX  // set __va_argsave.reg_args
        genc1(cv,0x89,(REX_W << 16) | modregrm(2,AX,AX),FLconst,6*8+8*16+4+4+8);

        pinholeopt(cv, NULL);
        useregs(mDX|mAX);
        c = cat(c,cv);
        }
    }

#if 0 && TARGET_LINUX
    if (gotref)
    {                                   // position independent reference
        c = cat(c, cod3_load_got());
    }
#endif

    return c;
}

/*******************************
 * Generate and return function epilog.
 * Output:
 *      retsize         Size of function epilog
 */

static targ_size_t spoff;

void epilog(block *b)
{   code *c;
    code *cr;
    code *ce;
    code *cpopds;
    unsigned reg;
    unsigned regx;                      // register that's not a return reg
    regm_t topop,regm;
    tym_t tyf,tym;
    int op;
    char farfunc;
    targ_size_t xlocalsize = localsize;

    c = CNIL;
    ce = b->Bcode;
    tyf = funcsym_p->ty();
    tym = tybasic(tyf);
    farfunc = tyfarfunc(tym);
    if (!(b->Bflags & BFLepilog))       // if no epilog code
        goto Lret;                      // just generate RET
    regx = (b->BC == BCret) ? AX : CX;

    spoff = 0;
    retsize = 0;

    if (tyf & mTYnaked)                 // if no prolog/epilog
        return;

    if (tym == TYifunc)
    {   static unsigned char ops2[] = { 0x07,0x1F,0x61,0xCF,0 };
        static unsigned char ops0[] = { 0x07,0x1F,0x5F,0x5E,
                                        0x5D,0x5B,0x5B,0x5A,
                                        0x59,0x58,0xCF,0 };
        unsigned char *p;

        c = genregs(c,0x8B,SP,BP);              // MOV SP,BP
        p = (config.target_cpu >= TARGET_80286) ? ops2 : ops0;
        do
            gen1(c,*p);
        while (*++p);
        goto Lopt;
    }

    if (config.flags & CFGtrace &&
        (!(config.flags4 & CFG4allcomdat) ||
         funcsym_p->Sclass == SCcomdat ||
         funcsym_p->Sclass == SCglobal ||
         (config.flags2 & CFG2comdat && SymInline(funcsym_p))
        )
       )
    {
        symbol *s = rtlsym[farfunc ? RTLSYM_TRACE_EPI_F : RTLSYM_TRACE_EPI_N];
        makeitextern(s);
        c = gencs(c,I16 ? 0x9A : CALL,0,FLfunc,s);      // CALLF _trace
        if (!I16)
            code_orflag(c,CFoff | CFselfrel);
        useregs((ALLREGS | mBP | mES) & ~s->Sregsaved);
    }

    if (usednteh & ~NTEHjmonitor && (config.exe == EX_NT || MARS))
        c = cat(c,nteh_epilog());

    cpopds = CNIL;
    if (tyf & mTYloadds)
    {   cpopds = gen1(cpopds,0x1F);             // POP DS
        c = cat(c,cpopds);
        spoff += intsize;
    }

    /* Pop all the general purpose registers saved on the stack
     * by the prolog code. Remember to do them in the reverse
     * order they were pushed.
     */
    reg = I64 ? R15 : DI;
    regm = 1 << reg;
    topop = fregsaved & ~mfuncreg;
#ifdef DEBUG
    if (topop & ~0xFFFF)
        printf("fregsaved = x%x, mfuncreg = x%x\n",fregsaved,mfuncreg);
#endif
    assert(!(topop & ~0xFFFF));
    while (topop)
    {   if (topop & regm)
        {   c = gen1(c,0x58 + (reg & 7));         // POP reg
            if (reg & 8)
                code_orrex(c, REX_B);
            topop &= ~regm;
            spoff += REGSIZE;
        }
        regm >>= 1;
        reg--;
    }

#if MARS
    if (usednteh & NTEHjmonitor)
    {
        regm_t retregs = 0;
        if (b->BC == BCretexp)
            retregs = regmask(b->Belem->Ety, tym);
        code *cn = nteh_monitor_epilog(retregs);
        c = cat(c,cn);
        xlocalsize += 8;
    }
#endif

    if (config.wflags & WFwindows && farfunc)
    {
        int wflags = config.wflags;
        if (wflags & WFreduced && !(tyf & mTYexport))
        {   // reduced prolog/epilog for non-exported functions
            wflags &= ~(WFdgroup | WFds | WFss);
            if (!(wflags & WFsaveds))
                goto L4;
        }

        if (localsize | usedalloca)
        {
            c = genc1(c,0x8D,modregrm(1,SP,6),FLconst,(targ_uns)-2); /* LEA SP,-2[BP] */
        }
        if (wflags & (WFsaveds | WFds | WFss | WFdgroup))
        {   if (cpopds)
                cpopds->Iop = NOP;              // don't need previous one
            c = gen1(c,0x1F);                   // POP DS
        }
        c = gen1(c,0x58 + BP);                  // POP BP
        if (config.wflags & WFincbp)
            gen1(c,0x48 + BP);                  // DEC BP
        assert(hasframe);
    }
    else
    {
        if (needframe || (xlocalsize && hasframe))
        {
        L4:
            assert(hasframe);
            if (xlocalsize | usedalloca)
            {   if (config.target_cpu >= TARGET_80286 &&
                    !(config.target_cpu >= TARGET_80386 &&
                     config.flags4 & CFG4speed)
                   )
                    c = gen1(c,0xC9);           // LEAVE
                else if (0 && xlocalsize == REGSIZE && !usedalloca && I32)
                {   // This doesn't work - I should figure out why
                    mfuncreg &= ~mask[regx];
                    c = gen1(c,0x58 + regx);    // POP regx
                    c = gen1(c,0x58 + BP);      // POP BP
                }
                else
                {   c = genregs(c,0x8B,SP,BP);  // MOV SP,BP
                    if (I64)
                        code_orrex(c, REX_W);   // MOV RSP,RBP
                    c = gen1(c,0x58 + BP);      // POP BP
                }
            }
            else
                c = gen1(c,0x58 + BP);          // POP BP
            if (config.wflags & WFincbp && farfunc)
                gen1(c,0x48 + BP);              // DEC BP
        }
        else if (xlocalsize == REGSIZE && (!I16 || b->BC == BCret))
        {   mfuncreg &= ~mask[regx];
            c = gen1(c,0x58 + regx);                    // POP regx
        }
        else if (xlocalsize)
        {
            c = genc2(c,0x81,modregrm(3,0,SP),xlocalsize);      // ADD SP,xlocalsize
            if (I64)
                code_orrex(c, REX_W);
        }
    }
    if (b->BC == BCret || b->BC == BCretexp)
    {
Lret:
        op = tyfarfunc(tym) ? 0xCA : 0xC2;
        if (tym == TYhfunc)
        {
            c = genc2(c,0xC2,0,4);                      // RET 4
        }
        else if (!typfunc(tym) ||                       // if caller cleans the stack
                 Poffset == 0)                          // or nothing pushed on the stack anyway
        {   op++;                                       // to a regular RET
            c = gen1(c,op);
        }
        else
        {   // Stack is always aligned on register size boundary
            Poffset = (Poffset + (REGSIZE - 1)) & ~(REGSIZE - 1);
            c = genc2(c,op,0,Poffset);          // RET Poffset
        }
    }

Lopt:
    // If last instruction in ce is ADD SP,imm, and first instruction
    // in c sets SP, we can dump the ADD.
    cr = code_last(ce);
    if (cr && c && !I64)
    {
        if (cr->Iop == 0x81 && cr->Irm == modregrm(3,0,SP))     // if ADD SP,imm
        {
            if (
                c->Iop == 0xC9 ||                                  // LEAVE
                (c->Iop == 0x8B && c->Irm == modregrm(3,SP,BP)) || // MOV SP,BP
                (c->Iop == 0x8D && c->Irm == modregrm(1,SP,6))     // LEA SP,-imm[BP]
               )
                cr->Iop = NOP;
            else if (c->Iop == 0x58 + BP)                       // if POP BP
            {   cr->Iop = 0x8B;
                cr->Irm = modregrm(3,SP,BP);                    // MOV SP,BP
            }
        }
#if 0   // These optimizations don't work if the called function
        // cleans off the stack.
        else if (c->Iop == 0xC3 && cr->Iop == CALL)     // CALL near
        {   cr->Iop = 0xE9;                             // JMP near
            c->Iop = NOP;
        }
        else if (c->Iop == 0xCB && cr->Iop == 0x9A)     // CALL far
        {   cr->Iop = 0xEA;                             // JMP far
            c->Iop = NOP;
        }
#endif
    }

    retsize += calcblksize(c);          // compute size of function epilog
    b->Bcode = cat(ce,c);
}

/*******************************
 * Return offset of SP from BP.
 */

targ_size_t cod3_spoff()
{
    return spoff + localsize;
}

/**********************************
 * Load value of _GLOBAL_OFFSET_TABLE_ into EBX
 */

code *cod3_load_got()
{
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
    code *c;
    code *cgot;

    c = genc2(NULL,CALL,0,0);   //     CALL L1
    gen1(c, 0x58 + BX);         // L1: POP EBX

                                //     ADD EBX,_GLOBAL_OFFSET_TABLE_+3
    symbol *gotsym = elfobj_getGOTsym();
    cgot = gencs(CNIL,0x81,0xC3,FLextern,gotsym);
    cgot->Iflags = CFoff;
    cgot->IEVoffset2 = 3;

    makeitextern(gotsym);
    return cat(c,cgot);
#else
    assert(0);
    return NULL;
#endif
}

code* gen_spill_reg(Symbol* s, bool toreg)
{
    code *c;
    code cs;
    regm_t keepmsk = toreg ? RMload : RMstore;
    int sz = type_size(s->Stype);

    elem* e = el_var(s); // so we can trick getlvalue() into working for us

    if (mask[s->Sreglsw] & XMMREGS)
    {   // Convert to save/restore of XMM register
        if (toreg)
            cs.Iop = xmmload(s->Stype->Tty);        // MOVSS/D xreg,mem
        else
            cs.Iop = xmmstore(s->Stype->Tty);       // MOVSS/D mem,xreg
        c = getlvalue(&cs,e,keepmsk);
        cs.orReg(s->Sreglsw - XMM0);
        c = gen(c,&cs);
    }
    else
    {
        cs.Iop = toreg ? 0x8B : 0x89; // MOV reg,mem[ESP] : MOV mem[ESP],reg
        cs.Iop ^= (sz == 1);
        c = getlvalue(&cs,e,keepmsk);
        cs.orReg(s->Sreglsw);
        if (I64 && sz == 1 && s->Sreglsw >= 4)
            cs.Irex |= REX;
        c = gen(c,&cs);
        if (sz > REGSIZE)
        {
            cs.setReg(s->Sregmsw);
            getlvalue_msw(&cs);
            c = gen(c,&cs);
        }
    }

    el_free(e);

    return c;
}

/****************************
 * Generate code for, and output a thunk.
 * Input:
 *      thisty  Type of this pointer
 *      p       ESP parameter offset to this pointer
 *      d       offset to add to 'this' pointer
 *      d2      offset from 'this' to vptr
 *      i       offset into vtbl[]
 */

void cod3_thunk(symbol *sthunk,symbol *sfunc,unsigned p,tym_t thisty,
        targ_size_t d,int i,targ_size_t d2)
{   code *c,*c1;
    targ_size_t thunkoffset;
    tym_t thunkty;

    cod3_align();

    /* Skip over return address */
    thunkty = tybasic(sthunk->ty());
#if TARGET_SEGMENTED
    if (tyfarfunc(thunkty))
        p += I32 ? 8 : tysize[TYfptr];          /* far function */
    else
#endif
        p += tysize[TYnptr];

    if (!I16)
    {
        /*
           Generate:
            ADD p[ESP],d
           For direct call:
            JMP sfunc
           For virtual call:
            MOV EAX, p[ESP]                     EAX = this
            MOV EAX, d2[EAX]                    EAX = this->vptr
            JMP i[EAX]                          jump to virtual function
         */
        unsigned reg = 0;
        if ((targ_ptrdiff_t)d < 0)
        {
            d = -d;
            reg = 5;                            // switch from ADD to SUB
        }
        if (thunkty == TYmfunc)
        {                                       // ADD ECX,d
            c = CNIL;
            if (d)
                c = genc2(c,0x81,modregrm(3,reg,CX),d);
        }
        else if (thunkty == TYjfunc || (I64 && thunkty == TYnfunc))
        {                                       // ADD EAX,d
            c = CNIL;
            if (d)
                c = genc2(c,0x81,modregrm(3,reg,I64 ? DI : AX),d);
        }
        else
        {
            c = genc(CNIL,0x81,modregrm(2,reg,4),
                FLconst,p,                      // to this
                FLconst,d);                     // ADD p[ESP],d
            c->Isib = modregrm(0,4,SP);
        }
        if (I64 && c)
            c->Irex |= REX_W;
    }
    else
    {
        /*
           Generate:
            MOV BX,SP
            ADD [SS:] p[BX],d
           For direct call:
            JMP sfunc
           For virtual call:
            MOV BX, p[BX]                       BX = this
            MOV BX, d2[BX]                      BX = this->vptr
            JMP i[BX]                           jump to virtual function
         */


        c = genregs(CNIL,0x89,SP,BX);                   /* MOV BX,SP    */
        c1 = genc(CNIL,0x81,modregrm(2,0,7),
            FLconst,p,                                  /* to this      */
            FLconst,d);                                 /* ADD p[BX],d  */
        if (config.wflags & WFssneds ||
            // If DS needs reloading from SS,
            // then assume SS != DS on thunk entry
            (config.wflags & WFss && LARGEDATA))
            c1->Iflags |= CFss;                         /* SS:          */
        c = cat(c,c1);
    }

    if ((i & 0xFFFF) != 0xFFFF)                 /* if virtual call      */
    {   code *c2,*c3;

#define FARTHIS (tysize(thisty) > REGSIZE)
#define FARVPTR FARTHIS

#if TARGET_SEGMENTED
        assert(thisty != TYvptr);               /* can't handle this case */
#endif

        if (!I16)
        {
            assert(!FARTHIS && !LARGECODE);
            if (thunkty == TYmfunc)     // if 'this' is in ECX
            {   c1 = CNIL;

                // MOV EAX,d2[ECX]
                c2 = genc1(CNIL,0x8B,modregrm(2,AX,CX),FLconst,d2);
            }
            else if (thunkty == TYjfunc)        // if 'this' is in EAX
            {   c1 = CNIL;

                // MOV EAX,d2[EAX]
                c2 = genc1(CNIL,0x8B,modregrm(2,AX,AX),FLconst,d2);
            }
            else
            {
                // MOV EAX,p[ESP]
                c1 = genc1(CNIL,0x8B,(modregrm(0,4,SP) << 8) | modregrm(2,AX,4),FLconst,(targ_uns) p);
                if (I64)
                    c1->Irex |= REX_W;

                // MOV EAX,d2[EAX]
                c2 = genc1(CNIL,0x8B,modregrm(2,AX,AX),FLconst,d2);
            }
            if (I64)
                code_orrex(c2, REX_W);
                                                        /* JMP i[EAX]   */
            c3 = genc1(CNIL,0xFF,modregrm(2,4,0),FLconst,(targ_uns) i);
        }
        else
        {
            /* MOV/LES BX,[SS:] p[BX]   */
            c1 = genc1(CNIL,(FARTHIS ? 0xC4 : 0x8B),modregrm(2,BX,7),FLconst,(targ_uns) p);
            if (config.wflags & WFssneds ||
                // If DS needs reloading from SS,
                // then assume SS != DS on thunk entry
                (config.wflags & WFss && LARGEDATA))
                c1->Iflags |= CFss;                     /* SS:          */

            /* MOV/LES BX,[ES:]d2[BX] */
            c2 = genc1(CNIL,(FARVPTR ? 0xC4 : 0x8B),modregrm(2,BX,7),FLconst,d2);
            if (FARTHIS)
                c2->Iflags |= CFes;                     /* ES:          */

                                                        /* JMP i[BX]    */
            c3 = genc1(CNIL,0xFF,modregrm(2,(LARGECODE ? 5 : 4),7),FLconst,(targ_uns) i);
            if (FARVPTR)
                c3->Iflags |= CFes;                     /* ES:          */
        }
        c = cat4(c,c1,c2,c3);
    }
    else
    {
        c1 = gencs(CNIL,(LARGECODE ? 0xEA : 0xE9),0,FLfunc,sfunc); /* JMP sfunc */
        c1->Iflags |= LARGECODE ? (CFseg | CFoff) : (CFselfrel | CFoff);
        c = cat(c,c1);
    }

    thunkoffset = Coffset;
    pinholeopt(c,NULL);
    codout(c);
    code_free(c);

    sthunk->Soffset = thunkoffset;
    sthunk->Ssize = Coffset - thunkoffset; /* size of thunk */
    sthunk->Sseg = cseg;
#if TARGET_LINUX || TARGET_OSX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
    objpubdef(cseg,sthunk,sthunk->Soffset);
#endif
    searchfixlist(sthunk);              /* resolve forward refs */
}

/*****************************
 * Assume symbol s is extern.
 */

void makeitextern(symbol *s)
{
        if (s->Sxtrnnum == 0)
        {       s->Sclass = SCextern;           /* external             */
                /*printf("makeitextern(x%x)\n",s);*/
                objextern(s);
        }
}


/*******************************
 * Replace JMPs in Bgotocode with JMP SHORTs whereever possible.
 * This routine depends on FLcode jumps to only be forward
 * referenced.
 * BFLjmpoptdone is set to TRUE if nothing more can be done
 * with this block.
 * Input:
 *      flag    !=0 means don't have correct Boffsets yet
 * Returns:
 *      number of bytes saved
 */

int branch(block *bl,int flag)
{ int bytesaved;
  code *c,*cn,*ct;
  targ_size_t offset,disp;
  targ_size_t csize;

  if (!flag)
      bl->Bflags |= BFLjmpoptdone;      // assume this will be all
  c = bl->Bcode;
  if (!c)
        return 0;
  bytesaved = 0;
  offset = bl->Boffset;                 /* offset of start of block     */
  while (1)
  {     unsigned char op;

        csize = calccodsize(c);
        cn = code_next(c);
        op = c->Iop;
        if ((op & ~0x0F) == 0x70 && c->Iflags & CFjmp16 ||
            op == JMP)
        {
          L1:
            switch (c->IFL2)
            {
                case FLblock:
                    if (flag)           // no offsets yet, don't optimize
                        goto L3;
                    disp = c->IEV2.Vblock->Boffset - offset - csize;

                    /* If this is a forward branch, and there is an aligned
                     * block intervening, it is possible that shrinking
                     * the jump instruction will cause it to be out of
                     * range of the target. This happens if the alignment
                     * prevents the target block from moving correspondingly
                     * closer.
                     */
                    if (disp >= 0x7F-4 && c->IEV2.Vblock->Boffset > offset)
                    {   /* Look for intervening alignment
                         */
                        for (block *b = bl->Bnext; b; b = b->Bnext)
                        {
                            if (b->Balign)
                            {
                                bl->Bflags &= ~BFLjmpoptdone;   // some JMPs left
                                goto L3;
                            }
                            if (b == c->IEV2.Vblock)
                                break;
                        }
                    }

                    break;

                case FLcode:
                {   code *cr;

                    disp = 0;

                    ct = c->IEV2.Vcode;         /* target of branch     */
                    assert(ct->Iflags & (CFtarg | CFtarg2));
                    for (cr = cn; cr; cr = code_next(cr))
                    {
                        if (cr == ct)
                            break;
                        disp += calccodsize(cr);
                    }

                    if (!cr)
                    {   // Didn't find it in forward search. Try backwards jump
                        int s = 0;
                        disp = 0;
                        for (cr = bl->Bcode; cr != cn; cr = code_next(cr))
                        {
                            assert(cr != NULL); // must have found it
                            if (cr == ct)
                                s = 1;
                            if (s)
                                disp += calccodsize(cr);
                        }
                    }

                    if (config.flags4 & CFG4optimized && !flag)
                    {
                        /* Propagate branch forward past junk   */
                        while (1)
                        {   if (ct->Iop == NOP ||
                                ct->Iop == (ESCAPE | ESClinnum))
                            {   ct = code_next(ct);
                                if (!ct)
                                    goto L2;
                            }
                            else
                            {   c->IEV2.Vcode = ct;
                                ct->Iflags |= CFtarg;
                                break;
                            }
                        }

                        /* And eliminate jmps to jmps   */
                        if ((op == ct->Iop || ct->Iop == JMP) &&
                            (op == JMP || c->Iflags & CFjmp16))
                        {   c->IFL2 = ct->IFL2;
                            c->IEV2.Vcode = ct->IEV2.Vcode;
                            /*printf("eliminating branch\n");*/
                            goto L1;
                        }
                     L2: ;
                    }
                }
                    break;

                default:
                    goto L3;
            }

            if (disp == 0)                      // bra to next instruction
            {   bytesaved += csize;
                c->Iop = NOP;                   // del branch instruction
                c->IEV2.Vcode = NULL;
                c = cn;
                if (!c)
                    break;
                continue;
            }
            else if ((targ_size_t)(targ_schar)(disp - 2) == (disp - 2) &&
                     (targ_size_t)(targ_schar)disp == disp)
            {
                if (op == JMP)
                {   c->Iop = JMPS;              // JMP SHORT
                    bytesaved += I16 ? 1 : 3;
                }
                else                            // else Jcond
                {   c->Iflags &= ~CFjmp16;      // a branch is ok
                    bytesaved += I16 ? 3 : 4;

                    // Replace a cond jump around a call to a function that
                    // never returns with a cond jump to that function.
                    if (config.flags4 & CFG4optimized &&
                        config.target_cpu >= TARGET_80386 &&
                        disp == (I16 ? 3 : 5) &&
                        cn &&
                        cn->Iop == CALL &&
                        cn->IFL2 == FLfunc &&
                        cn->IEVsym2->Sflags & SFLexit &&
                        !(cn->Iflags & (CFtarg | CFtarg2))
                       )
                    {
                        cn->Iop = 0x0F00 | ((c->Iop & 0x0F) ^ 0x81);
                        c->Iop = NOP;
                        c->IEV2.Vcode = NULL;
                        bytesaved++;

                        // If nobody else points to ct, we can remove the CFtarg
                        if (flag && ct)
                        {   code *cx;

                            for (cx = bl->Bcode; 1; cx = code_next(cx))
                            {
                                if (!cx)
                                {   ct->Iflags &= ~CFtarg;
                                    break;
                                }
                                if (cx->IEV2.Vcode == ct)
                                    break;
                            }
                        }
                    }
                }
                csize = calccodsize(c);
            }
            else
                bl->Bflags &= ~BFLjmpoptdone;   // some JMPs left
        }
L3:
        if (cn)
        {   offset += csize;
            c = cn;
        }
        else
            break;
  }
  //printf("bytesaved = x%x\n",bytesaved);
  return bytesaved;
}

/************************************************
 * Adjust all Soffset's of stack variables so they
 * are all relative to the frame pointer.
 */

#if MARS

void cod3_adjSymOffsets()
{   SYMIDX si;

    //printf("cod3_adjSymOffsets()\n");
    for (si = 0; si < globsym.top; si++)
    {   //printf("globsym.tab[%d] = %p\n",si,globsym.tab[si]);
        symbol *s = globsym.tab[si];

        switch (s->Sclass)
        {
            case SCparameter:
            case SCregpar:
//printf("s = '%s', Soffset = x%x, Poff = x%x, EBPtoESP = x%x\n", s->Sident, s->Soffset, Poff, EBPtoESP);
                s->Soffset += Poff;
if (0 && !(funcsym_p->Sfunc->Fflags3 & Fmember))
{
    if (!hasframe)
        s->Soffset += EBPtoESP;
    if (funcsym_p->Sfunc->Fflags3 & Fnested)
        s->Soffset += REGSIZE;
}
                break;
            case SCauto:
            case SCfastpar:
            case SCregister:
            case_auto:
//printf("s = '%s', Soffset = x%x, Aoff = x%x, BPoff = x%x EBPtoESP = x%x\n", s->Sident, s->Soffset, Aoff, BPoff, EBPtoESP);
//              if (!(funcsym_p->Sfunc->Fflags3 & Fnested))
                    s->Soffset += Aoff + BPoff;
                break;
            case SCbprel:
                break;
            default:
                continue;
        }
#if 0
        if (!hasframe)
            s->Soffset += EBPtoESP;
#endif
    }
}

#endif

/*******************************
 * Take symbol info in union ev and replace it with a real address
 * in Vpointer.
 */

void assignaddr(block *bl)
{
    int EBPtoESPsave = EBPtoESP;
    int hasframesave = hasframe;

    if (bl->Bflags & BFLoutsideprolog)
    {   EBPtoESP = -REGSIZE;
        hasframe = 0;
    }
    assignaddrc(bl->Bcode);
    hasframe = hasframesave;
    EBPtoESP = EBPtoESPsave;
}

void assignaddrc(code *c)
{
    int sn;
    symbol *s;
    unsigned char ins,rm;
    targ_size_t soff;
    targ_size_t base;

    base = EBPtoESP;
    for (; c; c = code_next(c))
    {
#ifdef DEBUG
        if (0)
        {       printf("assignaddrc()\n");
                c->print();
        }
        if (code_next(c) && code_next(code_next(c)) == c)
            assert(0);
#endif
        if (c->Iflags & CFvex)
            ins = vex_inssize(c);
        else if ((c->Iop & 0xFFFD00) == 0x0F3800)
            ins = inssize2[(c->Iop >> 8) & 0xFF];
        else if ((c->Iop & 0xFF00) == 0x0F00)
            ins = inssize2[c->Iop & 0xFF];
        else if ((c->Iop & 0xFF) == ESCAPE)
        {
            if (c->Iop == (ESCAPE | ESCadjesp))
            {
                //printf("adjusting EBPtoESP (%d) by %ld\n",EBPtoESP,c->IEV2.Vint);
                EBPtoESP += c->IEV1.Vint;
                c->Iop = NOP;
            }
            if (c->Iop == (ESCAPE | ESCframeptr))
            {   // Convert to load of frame pointer
                // c->Irm is the register to use
                if (hasframe)
                {   // MOV reg,EBP
                    c->Iop = 0x89;
                    if (c->Irm & 8)
                        c->Irex |= REX_B;
                    c->Irm = modregrm(3,BP,c->Irm & 7);
                }
                else
                {   // LEA reg,EBPtoESP[ESP]
                    c->Iop = 0x8D;
                    if (c->Irm & 8)
                        c->Irex |= REX_R;
                    c->Irm = modregrm(2,c->Irm & 7,4);
                    c->Isib = modregrm(0,4,SP);
                    c->Iflags = CFoff;
                    c->IFL1 = FLconst;
                    c->IEV1.Vuns = EBPtoESP;
                }
            }
            if (I64)
                c->Irex |= REX_W;
            continue;
        }
        else
            ins = inssize[c->Iop & 0xFF];
        if (!(ins & M) ||
            ((rm = c->Irm) & 0xC0) == 0xC0)
            goto do2;           /* if no first operand          */
        if (is32bitaddr(I32,c->Iflags))
        {

            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 4 && (c->Isib & 7) == 5 || (rm & 7) == 5))
               )
                goto do2;       /* if no first operand  */
        }
        else
        {
            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 6))
               )
                goto do2;       /* if no first operand  */
        }
        s = c->IEVsym1;
        switch (c->IFL1)
        {
#if OMFOBJ
            case FLdata:
                if (s->Sclass == SCcomdat)
                {   c->IFL1 = FLextern;
                    goto do2;
                }
#if MARS
                c->IEVseg1 = s->Sseg;
#else
                c->IEVseg1 = DATA;
#endif
                c->IEVpointer1 += s->Soffset;
                c->IFL1 = FLdatseg;
                goto do2;
            case FLudata:
#if MARS
                c->IEVseg1 = s->Sseg;
#else
                c->IEVseg1 = UDATA;
#endif
                c->IEVpointer1 += s->Soffset;
                c->IFL1 = FLdatseg;
                goto do2;
#else                                   // don't loose symbol information
            case FLdata:
            case FLudata:
            case FLtlsdata:
                c->IFL1 = FLextern;
                goto do2;
#endif
            case FLdatseg:
                c->IEVseg1 = DATA;
                goto do2;

#if TARGET_SEGMENTED
            case FLfardata:
            case FLcsdata:
#endif
            case FLpseudo:
                goto do2;

            case FLstack:
                //printf("Soffset = %d, EBPtoESP = %d, base = %d, pointer = %d\n",
                //s->Soffset,EBPtoESP,base,c->IEVpointer1);
                c->IEVpointer1 += s->Soffset + EBPtoESP - base - EEoffset;
                break;

            case FLreg:
            case FLauto:
                soff = Aoff;
            L1:
                if (s->Sflags & SFLunambig && !(s->Sflags & SFLread) && // if never loaded
                    !anyiasm &&
                    // if not optimized, leave it in for debuggability
                    (config.flags4 & CFG4optimized || !config.fulltypes))
                {   c->Iop = NOP;               // remove references to it
                    continue;
                }
                if (s->Sfl == FLreg && c->IEVpointer1 < 2)
                {       int reg = s->Sreglsw;

                        assert(!(s->Sregm & ~mask[reg]));
                        if (c->IEVpointer1 == 1)
                        {   assert(reg < 4);    /* must be a BYTEREGS   */
                            reg |= 4;           /* convert to high byte reg */
                        }
                        if (reg & 8)
                        {   assert(I64);
                            c->Irex |= REX_B;
                            reg &= 7;
                        }
                        c->Irm = (c->Irm & modregrm(0,7,0))
                                | modregrm(3,0,reg);
                        assert(c->Iop != LES && c->Iop != LEA);
                        goto do2;
                }
                else
                {   c->IEVpointer1 += s->Soffset + soff + BPoff;
                    if (s->Sflags & SFLunambig)
                        c->Iflags |= CFunambig;
            L2:
                    if (!hasframe)
                    {   /* Convert to ESP relative address instead of EBP */
                        unsigned char rm;

                        assert(!I16);
                        c->IEVpointer1 += EBPtoESP;
                        rm = c->Irm;
                        if ((rm & 7) == 4)              // if SIB byte
                        {
                            assert((c->Isib & 7) == BP);
                            assert((rm & 0xC0) != 0);
                            c->Isib = (c->Isib & ~7) | modregrm(0,0,SP);
                        }
                        else
                        {
                            assert((rm & 7) == 5);
                            c->Irm = (rm & modregrm(0,7,0))
                                    | modregrm(2,0,4);
                            c->Isib = modregrm(0,4,SP);
                        }
                    }
                }
                break;
            case FLpara:
                soff = Poff - BPoff;    // cancel out add of BPoff
                goto L1;
            case FLtmp:
                soff = Toff;
                goto L1;
            case FLfltreg:
                c->IEVpointer1 += Foff + BPoff;
                c->Iflags |= CFunambig;
                goto L2;
            case FLallocatmp:
                c->IEVpointer1 += AAoff + BPoff;
                goto L2;
            case FLbprel:
                c->IEVpointer1 += s->Soffset;
                break;
            case FLcs:
                sn = c->IEV1.Vuns;
                if (!CSE_loaded(sn))            // if never loaded
                {       c->Iop = NOP;
                        continue;
                }
                c->IEVpointer1 = sn * REGSIZE + CSoff + BPoff;
                c->Iflags |= CFunambig;
                goto L2;
            case FLregsave:
                sn = c->IEV1.Vuns;
                c->IEVpointer1 = sn + regsave.off + BPoff;
                c->Iflags |= CFunambig;
                goto L2;
            case FLndp:
#if MARS
                assert(c->IEV1.Vuns < NDP::savetop);
#endif
                c->IEVpointer1 = c->IEV1.Vuns * NDPSAVESIZE + NDPoff + BPoff;
                c->Iflags |= CFunambig;
                goto L2;
            case FLoffset:
                break;
            case FLlocalsize:
                c->IEVpointer1 += localsize;
                break;
            case FLconst:
            default:
                goto do2;
        }
        c->IFL1 = FLconst;
    do2:
        /* Ignore TEST (F6 and F7) opcodes      */
        if (!(ins & T)) goto done;              /* if no second operand */
        s = c->IEVsym2;
        switch (c->IFL2)
        {
#if ELFOBJ || MACHOBJ
            case FLdata:
            case FLudata:
            case FLtlsdata:
                c->IFL2 = FLextern;
                goto do2;
#else
            case FLdata:
                if (s->Sclass == SCcomdat)
                {   c->IFL2 = FLextern;
                    goto do2;
                }
#if MARS
                c->IEVseg2 = s->Sseg;
#else
                c->IEVseg2 = DATA;
#endif
                c->IEVpointer2 += s->Soffset;
                c->IFL2 = FLdatseg;
                goto done;
            case FLudata:
#if MARS
                c->IEVseg2 = s->Sseg;
#else
                c->IEVseg2 = UDATA;
#endif
                c->IEVpointer2 += s->Soffset;
                c->IFL2 = FLdatseg;
                goto done;
#endif
            case FLdatseg:
                c->IEVseg2 = DATA;
                goto done;
#if TARGET_SEGMENTED
            case FLcsdata:
            case FLfardata:
                goto done;
#endif
            case FLreg:
            case FLpseudo:
                assert(0);
                /* NOTREACHED */
            case FLauto:
                c->IEVpointer2 += s->Soffset + Aoff + BPoff;
                break;
            case FLpara:
                c->IEVpointer2 += s->Soffset + Poff;
                break;
            case FLtmp:
                c->IEVpointer2 += s->Soffset + Toff + BPoff;
                break;
            case FLfltreg:
                c->IEVpointer2 += Foff + BPoff;
                break;
            case FLallocatmp:
                c->IEVpointer2 += AAoff + BPoff;
                break;
            case FLbprel:
                c->IEVpointer2 += s->Soffset;
                break;

            case FLstack:
                c->IEVpointer2 += s->Soffset + EBPtoESP - base;
                break;

            case FLcs:
            case FLndp:
            case FLregsave:
                assert(0);
                /* NOTREACHED */

            case FLconst:
                break;

            case FLlocalsize:
                c->IEVpointer2 += localsize;
                break;

            default:
                goto done;
        }
        c->IFL2 = FLconst;
  done:
        ;
    }
}

/*******************************
 * Return offset from BP of symbol s.
 */

targ_size_t cod3_bpoffset(symbol *s)
{   targ_size_t offset;

    symbol_debug(s);
    offset = s->Soffset;
    switch (s->Sfl)
    {
        case FLpara:
            offset += Poff;
            break;
        case FLauto:
            offset += Aoff + BPoff;
            break;
        case FLtmp:
            offset += Toff + BPoff;
            break;
        default:
#ifdef DEBUG
            WRFL((enum FL)s->Sfl);
            symbol_print(s);
#endif
            assert(0);
    }
    assert(hasframe);
    return offset;
}


/*******************************
 * Find shorter versions of the same instructions.
 * Does these optimizations:
 *      replaces jmps to the next instruction with NOPs
 *      sign extension of modregrm displacement
 *      sign extension of immediate data (can't do it for OR, AND, XOR
 *              as the opcodes are not defined)
 *      short versions for AX EA
 *      short versions for reg EA
 * Input:
 *      b -> block for code (or NULL)
 */

void pinholeopt(code *c,block *b)
{ targ_size_t a;
  unsigned op,mod;
  unsigned char ins;
  int usespace;
  int useopsize;
  int space;
  block *bn;

#ifdef DEBUG
    static int tested; if (!tested) { tested++; pinholeopt_unittest(); }
#endif

#if 0
  code *cstart = c;
  if (debugc)
  {
      printf("+pinholeopt(%p)\n",c);
  }
#endif

  if (b)
  {     bn = b->Bnext;
        usespace = (config.flags4 & CFG4space && b->BC != BCasm);
        useopsize = (I16 || (config.flags4 & CFG4space && b->BC != BCasm));
  }
  else
  {     bn = NULL;
        usespace = (config.flags4 & CFG4space);
        useopsize = (I16 || config.flags4 & CFG4space);
  }
  for (; c; c = code_next(c))
  {
    L1:
        op = c->Iop;
        if (c->Iflags & CFvex)
            ins = vex_inssize(c);
        else if ((op & 0xFFFD00) == 0x0F3800)
            ins = inssize2[(op >> 8) & 0xFF];
        else if ((op & 0xFF00) == 0x0F00)
            ins = inssize2[op & 0xFF];
        else
            ins = inssize[op & 0xFF];
        if (ins & M)            // if modregrm byte
        {   int shortop = (c->Iflags & CFopsize) ? !I16 : I16;
            int local_BPRM = BPRM;

            if (c->Iflags & CFaddrsize)
                local_BPRM ^= 5 ^ 6;    // toggle between 5 and 6

            unsigned rm = c->Irm;
            unsigned reg = rm & modregrm(0,7,0);          // isolate reg field
            unsigned ereg = rm & 7;
            //printf("c = %p, op = %02x rm = %02x\n", c, op, rm);

            /* If immediate second operand      */
            if ((ins & T ||
                 ((op == 0xF6 || op == 0xF7) && (reg < modregrm(0,2,0) || reg > modregrm(0,3,0)))
                ) &&
                c->IFL2 == FLconst)
            {
                int flags = c->Iflags & CFpsw;      /* if want result in flags */
                targ_long u = c->IEV2.Vuns;
                if (ins & E)
                    u = (signed char) u;
                else if (shortop)
                    u = (short) u;

                // Replace CMP reg,0 with TEST reg,reg
                if ((op & 0xFE) == 0x80 &&              // 80 is CMP R8,imm8; 81 is CMP reg,imm
                    rm >= modregrm(3,7,AX) &&
                    u == 0)
                {       c->Iop = (op & 1) | 0x84;
                        c->Irm = modregrm(3,ereg,ereg);
                        if (c->Irex & REX_B)
                            c->Irex |= REX_R;
                        goto L1;
                }

                /* Optimize ANDs with an immediate constant             */
                if ((op == 0x81 || op == 0x80) && reg == modregrm(0,4,0))
                {
                    if (rm >= modregrm(3,4,AX))         // AND reg,imm
                    {
                        if (u == 0)
                        {       /* Replace with XOR reg,reg     */
                                c->Iop = 0x30 | (op & 1);
                                c->Irm = modregrm(3,ereg,ereg);
                                if (c->Irex & REX_B)
                                    c->Irex |= REX_R;
                                goto L1;
                        }
                        if (u == 0xFFFFFFFF && !flags)
                        {       c->Iop = NOP;
                                goto L1;
                        }
                    }
                    if (op == 0x81 && !flags)
                    {   // If we can do the operation in one byte

                        // If EA is not SI or DI
                        if ((rm < modregrm(3,4,SP) || I64) &&
                            (config.flags4 & CFG4space ||
                             config.target_cpu < TARGET_PentiumPro)
                           )
                        {
                            if ((u & 0xFFFFFF00) == 0xFFFFFF00)
                                goto L2;
                            else if (rm < modregrm(3,0,0) || (!c->Irex && ereg < 4))
                            {   if (!shortop)
                                {   if ((u & 0xFFFF00FF) == 0xFFFF00FF)
                                        goto L3;
                                }
                                else
                                {
                                    if ((u & 0xFF) == 0xFF)
                                        goto L3;
                                }
                            }
                        }
                        if (!shortop && useopsize)
                        {
                            if ((u & 0xFFFF0000) == 0xFFFF0000)
                            {   c->Iflags ^= CFopsize;
                                goto L1;
                            }
                            if ((u & 0xFFFF) == 0xFFFF && rm < modregrm(3,4,AX))
                            {   c->IEVoffset1 += 2; /* address MSW      */
                                c->IEV2.Vuns >>= 16;
                                c->Iflags ^= CFopsize;
                                goto L1;
                            }
                            if (rm >= modregrm(3,4,AX))
                            {
                                if (u == 0xFF && (rm <= modregrm(3,4,BX) || I64))
                                {   c->Iop = 0x0FB6;     // MOVZX
                                    c->Irm = modregrm(3,ereg,ereg);
                                    if (c->Irex & REX_B)
                                        c->Irex |= REX_R;
                                    goto L1;
                                }
                                if (u == 0xFFFF)
                                {   c->Iop = 0x0FB7;     // MOVZX
                                    c->Irm = modregrm(3,ereg,ereg);
                                    if (c->Irex & REX_B)
                                        c->Irex |= REX_R;
                                    goto L1;
                                }
                            }
                        }
                    }
                }

                /* Look for ADD,OR,SUB,XOR with u that we can eliminate */
                if (!flags &&
                    (op == 0x81 || op == 0x80) &&
                    (reg == modregrm(0,0,0) || reg == modregrm(0,1,0) ||  // ADD,OR
                     reg == modregrm(0,5,0) || reg == modregrm(0,6,0))    // SUB, XOR
                   )
                {
                        if (u == 0)
                        {
                                c->Iop = NOP;
                                goto L1;
                        }
                        if (u == ~0 && reg == modregrm(0,6,0))  /* XOR  */
                        {
                                c->Iop = 0xF6 | (op & 1);       /* NOT  */
                                c->Irm ^= modregrm(0,6^2,0);
                                goto L1;
                        }
                        if (!shortop &&
                            useopsize &&
                            op == 0x81 &&
                            (u & 0xFFFF0000) == 0 &&
                            (reg == modregrm(0,6,0) || reg == modregrm(0,1,0)))
                        {    c->Iflags ^= CFopsize;
                             goto L1;
                        }
                }

                /* Look for TEST or OR or XOR with an immediate constant */
                /* that we can replace with a byte operation            */
                if (op == 0xF7 && reg == modregrm(0,0,0) ||
                    op == 0x81 && reg == modregrm(0,6,0) && !flags ||
                    op == 0x81 && reg == modregrm(0,1,0))
                {
                    // See if we can replace a dword with a word
                    // (avoid for 32 bit instructions, because CFopsize
                    //  is too slow)
                    if (!shortop && useopsize)
                    {   if ((u & 0xFFFF0000) == 0)
                        {   c->Iflags ^= CFopsize;
                            goto L1;
                        }
                        /* If memory (not register) addressing mode     */
                        if ((u & 0xFFFF) == 0 && rm < modregrm(3,0,AX))
                        {   c->IEVoffset1 += 2; /* address MSW  */
                            c->IEV2.Vuns >>= 16;
                            c->Iflags ^= CFopsize;
                            goto L1;
                        }
                    }

                    // If EA is not SI or DI
                    if (rm < (modregrm(3,0,SP) | reg) &&
                        (usespace ||
                         config.target_cpu < TARGET_PentiumPro)
                       )
                    {
                        if ((u & 0xFFFFFF00) == 0)
                        {
                        L2: c->Iop--;           /* to byte instruction  */
                            c->Iflags &= ~CFopsize;
                            goto L1;
                        }
                        if (((u & 0xFFFF00FF) == 0 ||
                             (shortop && (u & 0xFF) == 0)) &&
                            (rm < modregrm(3,0,0) || (!c->Irex && ereg < 4)))
                        {
                        L3:
                            c->IEV2.Vuns >>= 8;
                            if (rm >= (modregrm(3,0,AX) | reg))
                                c->Irm |= 4;    /* AX->AH, BX->BH, etc. */
                            else
                                c->IEVoffset1 += 1;
                            goto L2;
                        }
                    }
#if 0
                    // BUG: which is right?
                    else if ((u & 0xFFFF0000) == 0)
#else
                    else if (0 && op == 0xF7 &&
                             rm >= modregrm(3,0,SP) &&
                             (u & 0xFFFF0000) == 0)
#endif
                        c->Iflags &= ~CFopsize;
                }

                // Try to replace TEST reg,-1 with TEST reg,reg
                if (op == 0xF6 && rm >= modregrm(3,0,AX) && rm <= modregrm(3,0,7)) // TEST regL,immed8
                {       if ((u & 0xFF) == 0xFF)
                        {
                           L4:  c->Iop = 0x84;          // TEST regL,regL
                                c->Irm = modregrm(3,ereg,ereg);
                                if (c->Irex & REX_B)
                                    c->Irex |= REX_R;
                                c->Iflags &= ~CFopsize;
                                goto L1;
                        }
                }
                if (op == 0xF7 && rm >= modregrm(3,0,AX) && rm <= modregrm(3,0,7) && (I64 || ereg < 4))
                {       if (u == 0xFF)
                                goto L4;
                        if ((u & 0xFFFF) == 0xFF00 && shortop && !c->Irex && ereg < 4)
                        {       ereg |= 4;                /* to regH      */
                                goto L4;
                        }
                }

                /* Look for sign extended immediate data */
                if ((signed char) u == u)
                {
                    if (op == 0x81)
                    {   if (reg != 0x08 && reg != 0x20 && reg != 0x30)
                            c->Iop = op = 0x83;         /* 8 bit sgn ext */
                    }
                    else if (op == 0x69)                /* IMUL rw,ew,dw */
                        c->Iop = op = 0x6B;             /* IMUL rw,ew,db */
                }

                // Look for SHIFT EA,imm8 we can replace with short form
                if (u == 1 && ((op & 0xFE) == 0xC0))
                    c->Iop |= 0xD0;

            } /* if immediate second operand */

            /* Look for AX short form */
            if (ins & A)
            {   if (rm == modregrm(0,AX,local_BPRM) &&
                    !(c->Irex & REX_R) &&               // and it's AX, not R8
                    (op & ~3) == 0x88 &&
                    !I64)
                {       op = ((op & 3) + 0xA0) ^ 2;
                        /* 8A-> A0 */
                        /* 8B-> A1 */
                        /* 88-> A2 */
                        /* 89-> A3 */
                        c->Iop = op;
                        c->IFL2 = c->IFL1;
                        c->IEV2 = c->IEV1;
                }

                /* Replace MOV REG1,REG2 with MOV EREG1,EREG2   */
                else if (!I16 &&
                         (op == 0x89 || op == 0x8B) &&
                         (rm & 0xC0) == 0xC0 &&
                         (!b || b->BC != BCasm)
                        )
                    c->Iflags &= ~CFopsize;

                // If rm is AX
                else if ((rm & modregrm(3,0,7)) == modregrm(3,0,AX) && !(c->Irex & (REX_R | REX_B)))
                {       switch (op)
                        {   case 0x80:  op = reg | 4; break;
                            case 0x81:  op = reg | 5; break;
                            case 0x87:  op = 0x90 + (reg>>3); break;    // XCHG
                            case 0xF6:
                                if (reg == 0)
                                    op = 0xA8;  /* TEST AL,immed8       */
                                break;
                            case 0xF7:
                                if (reg == 0)
                                    op = 0xA9;  /* TEST AX,immed16      */
                                break;
                        }
                        c->Iop = op;
                }
            }

            /* Look for reg short form */
            if ((ins & R) && (rm & 0xC0) == 0xC0)
            {   switch (op)
                {   case 0xC6:  op = 0xB0 + ereg; break;
                    case 0xC7:  op = 0xB8 + ereg; break;
                    case 0xFF:
                        switch (reg)
                        {   case 6<<3: op = 0x50+ereg; break;/* PUSH*/
                            case 0<<3: if (!I64) op = 0x40+ereg; break; /* INC*/
                            case 1<<3: if (!I64) op = 0x48+ereg; break; /* DEC*/
                        }
                        break;
                    case 0x8F:  op = 0x58 + ereg; break;
                    case 0x87:
                        if (reg == 0) op = 0x90 + ereg;
                        break;
                }
                c->Iop = op;
            }

            // Look to replace SHL reg,1 with ADD reg,reg
            if ((op & ~1) == 0xD0 &&
                     (rm & modregrm(3,7,0)) == modregrm(3,4,0) &&
                     config.target_cpu >= TARGET_80486)
            {
                c->Iop &= 1;
                c->Irm = (rm & modregrm(3,0,7)) | (ereg << 3);
                if (c->Irex & REX_B)
                    c->Irex |= REX_R;
                if (!(c->Iflags & CFpsw) && !I16)
                    c->Iflags &= ~CFopsize;
                goto L1;
            }

            /* Look for sign extended modregrm displacement, or 0
             * displacement.
             */

            if (((rm & 0xC0) == 0x80) && // it's a 16/32 bit disp
                c->IFL1 == FLconst)      // and it's a constant
            {
                a = c->IEVpointer1;
                if (a == 0 && (rm & 7) != local_BPRM &&         // if 0[disp]
                    !(local_BPRM == 5 && (rm & 7) == 4 && (c->Isib & 7) == BP)
                   )
                    c->Irm &= 0x3F;
                else if (!I16)
                {
                    if ((targ_size_t)(targ_schar)a == a)
                        c->Irm ^= 0xC0;                 /* do 8 sx      */
                }
                else if (((targ_size_t)(targ_schar)a & 0xFFFF) == (a & 0xFFFF))
                    c->Irm ^= 0xC0;                     /* do 8 sx      */
            }

            /* Look for LEA reg,[ireg], replace with MOV reg,ireg       */
            else if (op == 0x8D)
            {   rm = c->Irm & 7;
                mod = c->Irm & modregrm(3,0,0);
                if (mod == 0)
                {
                    if (!I16)
                    {
                        switch (rm)
                        {
                            case 4:
                            case 5:
                                break;
                            default:
                                c->Irm |= modregrm(3,0,0);
                                c->Iop = 0x8B;
                                break;
                        }
                    }
                    else
                    {
                        switch (rm)
                        {
                            case 4:     rm = modregrm(3,0,SI);  goto L6;
                            case 5:     rm = modregrm(3,0,DI);  goto L6;
                            case 7:     rm = modregrm(3,0,BX);  goto L6;
                            L6:     c->Irm = rm + reg;
                                    c->Iop = 0x8B;
                                    break;
                        }
                    }
                }

                /* replace LEA reg,0[BP] with MOV reg,BP        */
                else if (mod == modregrm(1,0,0) && rm == local_BPRM &&
                        c->IFL1 == FLconst && c->IEVpointer1 == 0)
                {       c->Iop = 0x8B;          /* MOV reg,BP   */
                        c->Irm = modregrm(3,0,BP) + reg;
                }
            }

            // Replace [R13] with 0[R13]
            if (c->Irex & REX_B && (c->Irm & modregrm(3,0,5)) == modregrm(0,0,5))
            {
                c->Irm |= modregrm(1,0,0);
                c->IFL1 = FLconst;
                c->IEVpointer1 = 0;
            }
        }
        else if (!(c->Iflags & CFvex))
        {
            switch (op)
            {
                default:
                    if ((op & ~0x0F) != 0x70)
                        break;
                case JMP:
                    switch (c->IFL2)
                    {   case FLcode:
                                if (c->IEV2.Vcode == code_next(c))
                                {       c->Iop = NOP;
                                        continue;
                                }
                                break;
                        case FLblock:
                                if (!code_next(c) && c->IEV2.Vblock == bn)
                                {       c->Iop = NOP;
                                        continue;
                                }
                                break;
                        case FLconst:
                        case FLfunc:
                        case FLextern:
                                break;
                        default:
#ifdef DEBUG
                                WRFL((enum FL)c->IFL2);
#endif
                                assert(0);
                    }
                    break;

                case 0x68:                      // PUSH immed16
                    if (c->IFL2 == FLconst)
                    {
                        targ_long u = c->IEV2.Vuns;
                        if (I64 ||
                            ((c->Iflags & CFopsize) ? I16 : I32))
                        {   // PUSH 32/64 bit operand
                            if (u == (signed char) u)
                                c->Iop = 0x6A;          // PUSH immed8
                        }
                        else // PUSH 16 bit operand
                        {   if ((short)u == (signed char) u)
                                c->Iop = 0x6A;          // PUSH immed8
                        }
                    }
                    break;
            }
        }
  }
#if 0
  if (1 || debugc) {
      printf("-pinholeopt(%p)\n",cstart);
        for (c = cstart; c; c = code_next(c))
            c->print();
  }
#endif
}

#ifdef DEBUG
STATIC void pinholeopt_unittest()
{
    //printf("pinholeopt_unittest()\n");
    struct CS { unsigned model,op,ea,ev1,ev2,flags; } tests[][2] =
    {
        // XOR reg,immed                            NOT regL
        {{ 16,0x81,modregrm(3,6,BX),0,0xFF,0 },    { 0,0xF6,modregrm(3,2,BX),0,0xFF }},

        // MOV 0[BX],3                               MOV [BX],3
        {{ 16,0xC7,modregrm(2,0,7),0,3},           { 0,0xC7,modregrm(0,0,7),0,3 }},

#if 0 // only if config.flags4 & CFG4space
        // TEST regL,immed8
        {{ 0,0xF6,modregrm(3,0,BX),0,0xFF,0 },    { 0,0x84,modregrm(3,BX,BX),0,0xFF }},
        {{ 0,0xF7,modregrm(3,0,BX),0,0xFF,0 },    { 0,0x84,modregrm(3,BX,BX),0,0xFF }},
        {{ 64,0xF6,modregrmx(3,0,R8),0,0xFF,0 },  { 0,0x84,modregxrmx(3,R8,R8),0,0xFF }},
        {{ 64,0xF7,modregrmx(3,0,R8),0,0xFF,0 },  { 0,0x84,modregxrmx(3,R8,R8),0,0xFF }},
#endif

        // PUSH immed => PUSH immed8
        {{ 0,0x68,0,0,0 },    { 0,0x6A,0,0,0 }},
        {{ 0,0x68,0,0,0x7F }, { 0,0x6A,0,0,0x7F }},
        {{ 0,0x68,0,0,0x80 }, { 0,0x68,0,0,0x80 }},
        {{ 16,0x68,0,0,0,CFopsize },    { 0,0x6A,0,0,0,CFopsize }},
        {{ 16,0x68,0,0,0x7F,CFopsize }, { 0,0x6A,0,0,0x7F,CFopsize }},
        {{ 16,0x68,0,0,0x80,CFopsize }, { 0,0x68,0,0,0x80,CFopsize }},
        {{ 16,0x68,0,0,0x10000,0 },     { 0,0x6A,0,0,0x10000,0 }},
        {{ 16,0x68,0,0,0x10000,CFopsize }, { 0,0x68,0,0,0x10000,CFopsize }},
        {{ 32,0x68,0,0,0,CFopsize },    { 0,0x6A,0,0,0,CFopsize }},
        {{ 32,0x68,0,0,0x7F,CFopsize }, { 0,0x6A,0,0,0x7F,CFopsize }},
        {{ 32,0x68,0,0,0x80,CFopsize }, { 0,0x68,0,0,0x80,CFopsize }},
        {{ 32,0x68,0,0,0x10000,CFopsize },    { 0,0x6A,0,0,0x10000,CFopsize }},
        {{ 32,0x68,0,0,0x8000,CFopsize }, { 0,0x68,0,0,0x8000,CFopsize }},
    };

    //config.flags4 |= CFG4space;
    for (int i = 0; i < sizeof(tests)/sizeof(tests[0]); i++)
    {   CS *pin  = &tests[i][0];
        CS *pout = &tests[i][1];
        code cs;
        memset(&cs, 0, sizeof(cs));
        if (pin->model)
        {
            if (I16 && pin->model != 16)
                continue;
            if (I32 && pin->model != 32)
                continue;
            if (I64 && pin->model != 64)
                continue;
        }
        //printf("[%d]\n", i);
        cs.Iop = pin->op;
        cs.Iea = pin->ea;
        cs.IFL1 = FLconst;
        cs.IFL2 = FLconst;
        cs.IEV1.Vuns = pin->ev1;
        cs.IEV2.Vuns = pin->ev2;
        cs.Iflags = pin->flags;
        pinholeopt(&cs, NULL);
        if (cs.Iop != pout->op)
        {   printf("[%d] Iop = x%02x, pout = x%02x\n", i, cs.Iop, pout->op);
            assert(0);
        }
        assert(cs.Iea == pout->ea);
        assert(cs.IEV1.Vuns == pout->ev1);
        assert(cs.IEV2.Vuns == pout->ev2);
        assert(cs.Iflags == pout->flags);
    }
}
#endif

/**************************
 * Compute jump addresses for FLcode.
 * Note: only works for forward referenced code.
 *       only direct jumps and branches are detected.
 *       LOOP instructions only work for backward refs.
 */

void jmpaddr(code *c)
{ code *ci,*cn,*ctarg,*cstart;
  targ_size_t ad;
  unsigned op;

  //printf("jmpaddr()\n");
  cstart = c;                           /* remember start of code       */
  while (c)
  {
        op = c->Iop;
        if (op <= 0xEB &&
            inssize[op] & T &&   // if second operand
            c->IFL2 == FLcode &&
            ((op & ~0x0F) == 0x70 || op == JMP || op == JMPS || op == JCXZ || op == CALL))
        {       ci = code_next(c);
                ctarg = c->IEV2.Vcode;  /* target code                  */
                ad = 0;                 /* IP displacement              */
                while (ci && ci != ctarg)
                {
                        ad += calccodsize(ci);
                        ci = code_next(ci);
                }
                if (!ci)
                    goto Lbackjmp;      // couldn't find it
                if (!I16 || op == JMP || op == JMPS || op == JCXZ || op == CALL)
                        c->IEVpointer2 = ad;
                else                    /* else conditional             */
                {       if (!(c->Iflags & CFjmp16))     /* if branch    */
                                c->IEVpointer2 = ad;
                        else            /* branch around a long jump    */
                        {       cn = code_next(c);
                                code_next(c) = code_calloc();
                                code_next(code_next(c)) = cn;
                                c->Iop = op ^ 1;        /* converse jmp */
                                c->Iflags &= ~CFjmp16;
                                c->IEVpointer2 = I16 ? 3 : 5;
                                cn = code_next(c);
                                cn->Iop = JMP;          /* long jump    */
                                cn->IFL2 = FLconst;
                                cn->IEVpointer2 = ad;
                        }
                }
                c->IFL2 = FLconst;
        }
        if (op == LOOP && c->IFL2 == FLcode)    /* backwards refs       */
        {
            Lbackjmp:
                ctarg = c->IEV2.Vcode;
                for (ci = cstart; ci != ctarg; ci = code_next(ci))
                        if (!ci || ci == c)
                                assert(0);
                ad = 2;                 /* - IP displacement            */
                while (ci != c)
                {       assert(ci);
                        ad += calccodsize(ci);
                        ci = code_next(ci);
                }
                c->IEVpointer2 = (-ad) & 0xFF;
                c->IFL2 = FLconst;
        }
        c = code_next(c);
  }
}

/*******************************
 * Calculate bl->Bsize.
 */

unsigned calcblksize(code *c)
{   unsigned size;

    for (size = 0; c; c = code_next(c))
    {
        unsigned sz = calccodsize(c);
        //printf("off=%02x, sz = %d, code %p: op=%02x\n", size, sz, c, c->Iop);
        size += sz;
    }
//printf("calcblksize(c = x%x) = %d\n", c, size);
    return size;
}

/*****************************
 * Calculate and return code size of a code.
 * Note that NOPs are sometimes used as markers, but are
 * never output. LINNUMs are never output.
 * Note: This routine must be fast. Profiling shows it is significant.
 */

unsigned calccodsize(code *c)
{   unsigned size;
    unsigned op;
    unsigned char rm,mod,ins;
    unsigned iflags;
    unsigned i32 = I32 || I64;
    unsigned a32 = i32;

#ifdef DEBUG
    assert((a32 & ~1) == 0);
#endif
    iflags = c->Iflags;
    op = c->Iop;
    if (iflags & CFvex)
    {
        ins = vex_inssize(c);
        size = ins & 7;
        goto Lmodrm;
    }
    else if ((op & 0xFF00) == 0x0F00 || (op & 0xFFFD00) == 0x0F3800)
        op = 0x0F;
    else
        op &= 0xFF;
    switch (op)
    {
        case 0x0F:
            if ((c->Iop & 0xFFFD00) == 0x0F3800)
            {   // 3 byte op ( 0F38-- or 0F3A-- )
                ins = inssize2[(c->Iop >> 8) & 0xFF];
                size = ins & 7;
                if (c->Iop & 0xFF000000)
                  size++;
            }
            else
            {   // 2 byte op ( 0F-- )
                ins = inssize2[c->Iop & 0xFF];
                size = ins & 7;
                if (c->Iop & 0xFF0000)
                  size++;
            }
            break;

        case NOP:
        case ESCAPE:
            size = 0;                   // since these won't be output
            goto Lret2;

        case ASM:
            if (c->Iflags == CFaddrsize)        // kludge for DA inline asm
                size = NPTRSIZE;
            else
                size = c->IEV1.as.len;
            goto Lret2;

        case 0xA1:
        case 0xA3:
            if (c->Irex)
            {
                size = 9;               // 64 bit immediate value for MOV to/from RAX
                goto Lret;
            }
            goto Ldefault;

        case 0xF6:                      /* TEST mem8,immed8             */
            ins = inssize[op];
            size = ins & 7;
            if (i32)
                size = inssize32[op];
            if ((c->Irm & (7<<3)) == 0)
                size++;                 /* size of immed8               */
            break;

        case 0xF7:
            ins = inssize[op];
            size = ins & 7;
            if (i32)
                size = inssize32[op];
            if ((c->Irm & (7<<3)) == 0)
                size += (i32 ^ ((iflags & CFopsize) !=0)) ? 4 : 2;
            break;

        default:
        Ldefault:
            ins = inssize[op];
            size = ins & 7;
            if (i32)
                size = inssize32[op];
    }

    if (iflags & (CFwait | CFopsize | CFaddrsize | CFSEG))
    {
        if (iflags & CFwait)    // if add FWAIT prefix
            size++;
        if (iflags & CFSEG)     // if segment override
            size++;

        // If the instruction has a second operand that is not an 8 bit,
        // and the operand size prefix is present, then fix the size computation
        // because the operand size will be different.
        // Walter, I had problems with this bit at the end.  There can still be
        // an ADDRSIZE prefix for these and it does indeed change the operand size.

        if (iflags & (CFopsize | CFaddrsize))
        {
            if ((ins & (T|E)) == T)
            {
                if ((op & 0xAC) == 0xA0)
                {
                    if (iflags & CFaddrsize && !I64)
                    {   if (I32)
                            size -= 2;
                        else
                            size += 2;
                    }
                }
                else if (iflags & CFopsize)
                {   if (I16)
                        size += 2;
                    else
                        size -= 2;
                }
            }
            if (iflags & CFaddrsize)
            {   if (!I64)
                    a32 ^= 1;
                size++;
            }
            if (iflags & CFopsize)
                size++;                         /* +1 for OPSIZE prefix         */
        }
    }

Lmodrm:
    if ((op & ~0x0F) == 0x70)
    {   if (iflags & CFjmp16)           // if long branch
            size += I16 ? 3 : 4;        // + 3(4) bytes for JMP
    }
    else if (ins & M)                   // if modregrm byte
    {
        rm = c->Irm;
        mod = rm & 0xC0;
        if (a32 || I64)
        {   // 32 bit addressing
            if (issib(rm))
                size++;
            switch (mod)
            {   case 0:
                    if (issib(rm) && (c->Isib & 7) == 5 ||
                        (rm & 7) == 5)
                        size += 4;      /* disp32                       */
                    if (c->Irex & REX_B && (rm & 7) == 5)
                        /* Instead of selecting R13, this mode is an [RIP] relative
                         * address. Although valid, it's redundant, and should not
                         * be generated. Instead, generate 0[R13] instead of [R13].
                         */
                        assert(0);
                    break;
                case 0x40:
                    size++;             /* disp8                        */
                    break;
                case 0x80:
                    size += 4;          /* disp32                       */
                    break;
            }
        }
        else
        {   // 16 bit addressing
            if (mod == 0x40)            /* 01: 8 bit displacement       */
                size++;
            else if (mod == 0x80 || (mod == 0 && (rm & 7) == 6))
                size += 2;
        }
    }

Lret:
    if (!(iflags & CFvex) && c->Irex)
    {   size++;
        if (c->Irex & REX_W && (op & ~7) == 0xB8)
            size += 4;
    }
Lret2:
    //printf("op = x%02x, size = %d\n",op,size);
    return size;
}

/********************************
 * Return !=0 if codes match.
 */

#if 0

int code_match(code *c1,code *c2)
{   code cs1,cs2;
    unsigned char ins;

    if (c1 == c2)
        goto match;
    cs1 = *c1;
    cs2 = *c2;
    if (cs1.Iop != cs2.Iop)
        goto nomatch;
    switch (cs1.Iop)
    {
        case ESCAPE | ESCctor:
        case ESCAPE | ESCdtor:
            goto nomatch;

        case NOP:
            goto match;

        case ASM:
            if (cs1.IEV1.as.len == cs2.IEV1.as.len &&
                memcmp(cs1.IEV1.as.bytes,cs2.IEV1.as.bytes,cs1.EV1.as.len) == 0)
                goto match;
            else
                goto nomatch;

        default:
            if ((cs1.Iop & 0xFF) == ESCAPE)
                goto match;
            break;
    }
    if (cs1.Iflags != cs2.Iflags)
        goto nomatch;

    ins = inssize[cs1.Iop & 0xFF];
    if ((cs1.Iop & 0xFFFD00) == 0x0F3800)
    {
        ins = inssize2[(cs1.Iop >> 8) & 0xFF];
    }
    else if ((cs1.Iop & 0xFF00) == 0x0F00)
    {
        ins = inssize2[cs1.Iop & 0xFF];
    }

    if (ins & M)                // if modregrm byte
    {
        if (cs1.Irm != cs2.Irm)
            goto nomatch;
        if ((cs1.Irm & 0xC0) == 0xC0)
            goto do2;
        if (is32bitaddr(I32,cs1.Iflags))
        {
            if (issib(cs1.Irm) && cs1.Isib != cs2.Isib)
                goto nomatch;
            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 4 && (c->Isib & 7) == 5 || (rm & 7) == 5))
               )
                goto do2;       /* if no first operand  */
        }
        else
        {
            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 6))
               )
                goto do2;       /* if no first operand  */
        }
        if (cs1.IFL1 != cs2.IFL1)
            goto nomatch;
        if (flinsymtab[cs1.IFL1] && cs1.IEVsym1 != cs2.IEVsym1)
            goto nomatch;
        if (cs1.IEVoffset1 != cs2.IEVoffset1)
            goto nomatch;
    }

do2:
    if (!(ins & T))                     // if no second operand
        goto match;
    if (cs1.IFL2 != cs2.IFL2)
        goto nomatch;
    if (flinsymtab[cs1.IFL2] && cs1.IEVsym2 != cs2.IEVsym2)
        goto nomatch;
    if (cs1.IEVoffset2 != cs2.IEVoffset2)
        goto nomatch;

match:
    return 1;

nomatch:
    return 0;
}

#endif

/**************************
 * Write code to intermediate file.
 * Code starts at offset.
 * Returns:
 *      addr of end of code
 */

static targ_size_t offset;              /* to save code use a global    */
static char bytes[100];
static char *pgen;

#define GEN(c)          (*pgen++ = (c))
#define GENP(n,p)       (memcpy(pgen,(p),(n)), pgen += (n))
#if ELFOBJ || MACHOBJ || _MSC_VER
#define FLUSH()         if (pgen-bytes) cod3_flush()
#else
#define FLUSH()         ((pgen - bytes) && cod3_flush())
#endif
#define OFFSET()        (offset + (pgen - bytes))

STATIC void cod3_flush()
{
    // Emit accumulated bytes to code segment
#ifdef DEBUG
    assert(pgen - bytes < sizeof(bytes));
#endif
    offset += obj_bytes(cseg,offset,pgen - bytes,bytes);
    pgen = bytes;
}

unsigned codout(code *c)
{ unsigned op;
  unsigned char rm,mod;
  unsigned char ins;
  code *cn;
  unsigned flags;
  symbol *s;

#ifdef DEBUG
  if (debugc) printf("codout(%p), Coffset = x%llx\n",c,(unsigned long long)Coffset);
#endif

  pgen = bytes;
  offset = Coffset;
  for (; c; c = code_next(c))
  {
#ifdef DEBUG
        if (debugc) { printf("off=%02lx, sz=%ld, ",(long)OFFSET(),(long)calccodsize(c)); c->print(); }
        unsigned startoffset = OFFSET();
#endif
        op = c->Iop;
        ins = inssize[op & 0xFF];
        switch (op & 0xFF)
        {   case ESCAPE:
                /* Check for SSE4 opcode v/pmaxuw xmm1,xmm2/m128 */
                if(op == 0x660F383E || c->Iflags & CFvex) break;

                switch (op & 0xFFFF00)
                {   case ESClinnum:
                        /* put out line number stuff    */
                        objlinnum(c->IEV1.Vsrcpos,OFFSET());
                        break;
#if SCPP
#if 1
                    case ESCctor:
                    case ESCdtor:
                    case ESCoffset:
                        if (config.exe != EX_NT)
                            except_pair_setoffset(c,OFFSET() - funcoffset);
                        break;
                    case ESCmark:
                    case ESCrelease:
                    case ESCmark2:
                    case ESCrelease2:
                        break;
#else
                    case ESCctor:
                        except_push(OFFSET() - funcoffset,c->IEV1.Vtor,NULL);
                        break;
                    case ESCdtor:
                        except_pop(OFFSET() - funcoffset,c->IEV1.Vtor,NULL);
                        break;
                    case ESCmark:
                        except_mark();
                        break;
                    case ESCrelease:
                        except_release();
                        break;
#endif
#endif
                }
#ifdef DEBUG
                assert(calccodsize(c) == 0);
#endif
                continue;
            case NOP:                   /* don't send them out          */
                if (op != NOP)
                    break;
#ifdef DEBUG
                assert(calccodsize(c) == 0);
#endif
                continue;
            case ASM:
                if (op != ASM)
                    break;
                FLUSH();
                if (c->Iflags == CFaddrsize)    // kludge for DA inline asm
                {
                    do32bit(FLblockoff,&c->IEV1,0);
                }
                else
                {
                    offset += obj_bytes(cseg,offset,c->IEV1.as.len,c->IEV1.as.bytes);
                }
#ifdef DEBUG
                assert(calccodsize(c) == c->IEV1.as.len);
#endif
                continue;
        }
        flags = c->Iflags;

        // See if we need to flush (don't have room for largest code sequence)
        if (pgen - bytes > sizeof(bytes) - (1+4+4+8+8))
            FLUSH();

        // see if we need to put out prefix bytes
        if (flags & (CFwait | CFPREFIX | CFjmp16))
        {   int override;

            if (flags & CFwait)
                GEN(0x9B);                      // FWAIT
                                                /* ? SEGES : SEGSS      */
            switch (flags & CFSEG)
            {   case CFes:      override = SEGES;       goto segover;
                case CFss:      override = SEGSS;       goto segover;
                case CFcs:      override = SEGCS;       goto segover;
                case CFds:      override = SEGDS;       goto segover;
                case CFfs:      override = SEGFS;       goto segover;
                case CFgs:      override = SEGGS;       goto segover;
                segover:        GEN(override);
                                break;
            }

            if (flags & CFaddrsize)
                GEN(0x67);

            // Do this last because of instructions like ADDPD
            if (flags & CFopsize)
                GEN(0x66);                      /* operand size         */

            if ((op & ~0x0F) == 0x70 && flags & CFjmp16) /* long condit jmp */
            {
                if (!I16)
                {   // Put out 16 bit conditional jump
                    c->Iop = op = 0x0F00 | (0x80 | (op & 0x0F));
                }
                else
                {
                    cn = code_calloc();
                    /*cxcalloc++;*/
                    code_next(cn) = code_next(c);
                    code_next(c) = cn;          // link into code
                    cn->Iop = JMP;              // JMP block
                    cn->IFL2 = c->IFL2;
                    cn->IEV2.Vblock = c->IEV2.Vblock;
                    c->Iop = op ^= 1;           // toggle condition
                    c->IFL2 = FLconst;
                    c->IEVpointer2 = I16 ? 3 : 5; // skip over JMP block
                    c->Iflags &= ~CFjmp16;
                }
            }
        }

        if (flags & CFvex)
        {
            if (flags & CFvex3)
            {
                GEN(0xC4);
                GEN(VEX3_B1(c->Ivex));
                GEN(VEX3_B2(c->Ivex));
                GEN(c->Ivex.op);
            }
            else
            {
                GEN(0xC5);
                GEN(VEX2_B1(c->Ivex));
                GEN(c->Ivex.op);
            }
            ins = vex_inssize(c);
            goto Lmodrm;
        }

        if (op > 0xFF)
        {
            if ((op & 0xFFFD00) == 0x0F3800)
                ins = inssize2[(op >> 8) & 0xFF];
            else if ((op & 0xFF00) == 0x0F00)
                ins = inssize2[op & 0xFF];

            if (op & 0xFF000000)
            {
                unsigned char op1 = op >> 24;
                if (op1 == 0xF2 || op1 == 0xF3 || op1 == 0x66)
                {
                    GEN(op1);
                    if (c->Irex)
                        GEN(c->Irex | REX);
                }
                else
                {
                    if (c->Irex)
                        GEN(c->Irex | REX);
                    GEN(op1);
                }
                GEN((op >> 16) & 0xFF);
                GEN((op >> 8) & 0xFF);
                GEN(op & 0xFF);
            }
            else if (op & 0xFF0000)
            {
                unsigned char op1 = op >> 16;
                if (op1 == 0xF2 || op1 == 0xF3 || op1 == 0x66)
                {
                    GEN(op1);
                    if (c->Irex)
                        GEN(c->Irex | REX);
                }
                else
                {
                    if (c->Irex)
                        GEN(c->Irex | REX);
                    GEN(op1);
                }
                GEN((op >> 8) & 0xFF);
                GEN(op & 0xFF);
            }
            else
            {
                if (c->Irex)
                    GEN(c->Irex | REX);
                GEN((op >> 8) & 0xFF);
                GEN(op & 0xFF);
            }
        }
        else
        {
            if (c->Irex)
                GEN(c->Irex | REX);
            GEN(op);
        }
  Lmodrm:
        if (ins & M)            /* if modregrm byte             */
        {
            rm = c->Irm;
            GEN(rm);

            // Look for an address size override when working with the
            // MOD R/M and SIB bytes

            if (is32bitaddr( I32, flags))
            {
                if (issib(rm))
                    GEN(c->Isib);
                switch (rm & 0xC0)
                {   case 0x40:
                        do8bit((enum FL) c->IFL1,&c->IEV1);     // 8 bit
                        break;
                    case 0:
                        if (!(issib(rm) && (c->Isib & 7) == 5 ||
                              (rm & 7) == 5))
                            break;
                    case 0x80:
                    {   int flags = CFoff;
                        targ_size_t val = 0;
                        if (I64)
                        {
                            if ((rm & modregrm(3,0,7)) == modregrm(0,0,5))      // if disp32[RIP]
                            {   flags |= CFpc32;
                                val = -4;
                                unsigned reg = rm & modregrm(0,7,0);
                                if (ins & T ||
                                    ((op == 0xF6 || op == 0xF7) && (reg == modregrm(0,0,0) || reg == modregrm(0,1,0))))
                                {   if (ins & E)
                                        val = -5;
                                    else if (c->Iflags & CFopsize)
                                        val = -6;
                                    else
                                        val = -8;
                                }
#if TARGET_OSX
                                // Mach-O linkage already takes the 4 byte size into account
                                val += 4;
#endif
                            }
                        }
                        do32bit((enum FL)c->IFL1,&c->IEV1,flags,val);
                        break;
                    }
                }
            }
            else
            {
                switch (rm & 0xC0)
                {   case 0x40:
                        do8bit((enum FL) c->IFL1,&c->IEV1);     // 8 bit
                        break;
                    case 0:
                        if ((rm & 7) != 6)
                            break;
                    case 0x80:
                        do16bit((enum FL)c->IFL1,&c->IEV1,CFoff);
                        break;
                }
            }
        }
        else
        {
            if (op == 0xC8)
                do16bit((enum FL)c->IFL1,&c->IEV1,0);
        }
        flags &= CFseg | CFoff | CFselfrel;
        if (ins & T)                    /* if second operand            */
        {       if (ins & E)            /* if data-8                    */
                    do8bit((enum FL) c->IFL2,&c->IEV2);
                else if (!I16)
                {
                    switch (op)
                    {   case 0xC2:              /* RETN imm16           */
                        case 0xCA:              /* RETF imm16           */
                        do16:
                            do16bit((enum FL)c->IFL2,&c->IEV2,flags);
                            break;

                        case 0xA1:
                        case 0xA3:
                            if (I64 && c->Irex)
                            {
                        do64:
                                do64bit((enum FL)c->IFL2,&c->IEV2,flags);
                                break;
                            }
                        case 0xA0:              /* MOV AL,byte ptr []   */
                        case 0xA2:
                            if (c->Iflags & CFaddrsize && !I64)
                                goto do16;
                            else
                        do32:
                                do32bit((enum FL)c->IFL2,&c->IEV2,flags);
                            break;
                        case 0x9A:
                        case 0xEA:
                            if (c->Iflags & CFopsize)
                                goto ptr1616;
                            else
                                goto ptr1632;

                        case 0x68:              // PUSH immed32
                            if ((enum FL)c->IFL2 == FLblock)
                            {
                                c->IFL2 = FLblockoff;
                                goto do32;
                            }
                            else
                                goto case_default;

                        case CALL:              // CALL rel
                        case JMP:               // JMP  rel
                            flags |= CFselfrel;
                            goto case_default;

                        default:
                            if ((op|0xF) == 0x0F8F) // Jcc rel16 rel32
                                flags |= CFselfrel;
                            if (I64 && (op & ~7) == 0xB8 && c->Irex & REX_W)
                                goto do64;
                        case_default:
                            if (c->Iflags & CFopsize)
                                goto do16;
                            else
                                goto do32;
                            break;
                    }
                }
                else
                {
                    switch (op) {
                        case 0xC2:
                        case 0xCA:
                            goto do16;
                        case 0xA0:
                        case 0xA1:
                        case 0xA2:
                        case 0xA3:
                            if (c->Iflags & CFaddrsize)
                                goto do32;
                            else
                                goto do16;
                            break;
                        case 0x9A:
                        case 0xEA:
                            if (c->Iflags & CFopsize)
                                goto ptr1632;
                            else
                                goto ptr1616;

                        ptr1616:
                        ptr1632:
                            //assert(c->IFL2 == FLfunc);
                            FLUSH();
                            if (c->IFL2 == FLdatseg)
                            {
                                reftodatseg(cseg,offset,c->IEVpointer2,
                                        c->IEVseg2,flags);
                                offset += 4;
                            }
                            else
                            {
                                s = c->IEVsym2;
                                offset += reftoident(cseg,offset,s,0,flags);
                            }
                            break;

                        case 0x68:              // PUSH immed16
                            if ((enum FL)c->IFL2 == FLblock)
                            {   c->IFL2 = FLblockoff;
                                goto do16;
                            }
                            else
                                goto case_default16;

                        case CALL:
                        case JMP:
                            flags |= CFselfrel;
                        default:
                        case_default16:
                            if (c->Iflags & CFopsize)
                                goto do32;
                            else
                                goto do16;
                            break;
                    }
                }
        }
        else if (op == 0xF6)            /* TEST mem8,immed8             */
        {       if ((rm & (7<<3)) == 0)
                        do8bit((enum FL)c->IFL2,&c->IEV2);
        }
        else if (op == 0xF7)
        {   if ((rm & (7<<3)) == 0)     /* TEST mem16/32,immed16/32     */
            {
                if ((I32 || I64) ^ ((c->Iflags & CFopsize) != 0))
                    do32bit((enum FL)c->IFL2,&c->IEV2,flags);
                else
                    do16bit((enum FL)c->IFL2,&c->IEV2,flags);
            }
        }
#ifdef DEBUG
        if (OFFSET() - startoffset != calccodsize(c))
        {
            printf("actual: %d, calc: %d\n", (int)(OFFSET() - startoffset), (int)calccodsize(c));
            c->print();
            assert(0);
        }
#endif
    }
    FLUSH();
    Coffset = offset;
    //printf("-codout(), Coffset = x%x\n", Coffset);
    return offset;                      /* ending address               */
}


STATIC void do64bit(enum FL fl,union evc *uev,int flags)
{   char *p;
    symbol *s;
    targ_size_t ad;

    assert(I64);
    switch (fl)
    {
        case FLconst:
            ad = * (targ_size_t *) uev;
        L1:
            GENP(8,&ad);
            return;
        case FLdatseg:
            FLUSH();
            reftodatseg(cseg,offset,uev->_EP.Vpointer,uev->_EP.Vseg,CFoffset64 | flags);
            break;
        case FLframehandler:
            framehandleroffset = OFFSET();
            ad = 0;
            goto L1;
        case FLswitch:
            FLUSH();
            ad = uev->Vswitch->Btableoffset;
            if (config.flags & CFGromable)
                    reftocodseg(cseg,offset,ad);
            else
                    reftodatseg(cseg,offset,ad,JMPSEG,CFoff);
            break;
#if TARGET_SEGMENTED
        case FLcsdata:
        case FLfardata:
#if DEBUG
            symbol_print(uev->sp.Vsym);
#endif
#endif
            // NOTE: In ELFOBJ all symbol refs have been tagged FLextern
            // strings and statics are treated like offsets from a
            // un-named external with is the start of .rodata or .data
        case FLextern:                      /* external data symbol         */
        case FLtlsdata:
#if TARGET_LINUX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
        case FLgot:
        case FLgotoff:
#endif
            FLUSH();
            s = uev->sp.Vsym;               /* symbol pointer               */
            reftoident(cseg,offset,s,uev->sp.Voffset,CFoffset64 | flags);
            break;

#if TARGET_OSX
        case FLgot:
            funcsym_p->Slocalgotoffset = OFFSET();
            ad = 0;
            goto L1;
#endif

        case FLfunc:                        /* function call                */
            s = uev->sp.Vsym;               /* symbol pointer               */
            assert(TARGET_SEGMENTED || !tyfarfunc(s->ty()));
            FLUSH();
            reftoident(cseg,offset,s,0,CFoffset64 | flags);
            break;

        case FLblock:                       /* displacement to another block */
            ad = uev->Vblock->Boffset - OFFSET() - 4;
            //printf("FLblock: funcoffset = %x, OFFSET = %x, Boffset = %x, ad = %x\n", funcoffset, OFFSET(), uev->Vblock->Boffset, ad);
            goto L1;

        case FLblockoff:
            FLUSH();
            assert(uev->Vblock);
            //printf("FLblockoff: offset = %x, Boffset = %x, funcoffset = %x\n", offset, uev->Vblock->Boffset, funcoffset);
            reftocodseg(cseg,offset,uev->Vblock->Boffset);
            break;

        default:
#ifdef DEBUG
            WRFL(fl);
#endif
            assert(0);
    }
    offset += 8;
}


STATIC void do32bit(enum FL fl,union evc *uev,int flags, targ_size_t val)
{ char *p;
  symbol *s;
  targ_size_t ad;

  //printf("do32bit(flags = x%x)\n", flags);
  switch (fl)
  {
    case FLconst:
        assert(sizeof(targ_size_t) == 4 || sizeof(targ_size_t) == 8);
        ad = * (targ_size_t *) uev;
    L1:
        GENP(4,&ad);
        return;
    case FLdatseg:
        FLUSH();
        reftodatseg(cseg,offset,uev->_EP.Vpointer,uev->_EP.Vseg,flags);
        break;
    case FLframehandler:
        framehandleroffset = OFFSET();
        ad = 0;
        goto L1;
    case FLswitch:
        FLUSH();
        ad = uev->Vswitch->Btableoffset;
        if (config.flags & CFGromable)
                reftocodseg(cseg,offset,ad);
        else
                reftodatseg(cseg,offset,ad,JMPSEG,CFoff);
        break;
#if TARGET_SEGMENTED
    case FLcsdata:
    case FLfardata:
#if DEBUG
        symbol_print(uev->sp.Vsym);
#endif
#endif
        // NOTE: In ELFOBJ all symbol refs have been tagged FLextern
        // strings and statics are treated like offsets from a
        // un-named external with is the start of .rodata or .data
    case FLextern:                      /* external data symbol         */
    case FLtlsdata:
#if TARGET_LINUX || TARGET_FREEBSD || TARGET_OPENBSD || TARGET_SOLARIS
    case FLgot:
    case FLgotoff:
#endif
        FLUSH();
        s = uev->sp.Vsym;               /* symbol pointer               */
        reftoident(cseg,offset,s,uev->sp.Voffset + val,flags);
        break;

#if TARGET_OSX
    case FLgot:
        funcsym_p->Slocalgotoffset = OFFSET();
        ad = 0;
        goto L1;
#endif

    case FLfunc:                        /* function call                */
        s = uev->sp.Vsym;               /* symbol pointer               */
#if TARGET_SEGMENTED
        if (tyfarfunc(s->ty()))
        {       /* Large code references are always absolute    */
                FLUSH();
                offset += reftoident(cseg,offset,s,0,flags) - 4;
        }
        else if (s->Sseg == cseg &&
                 (s->Sclass == SCstatic || s->Sclass == SCglobal) &&
                 s->Sxtrnnum == 0 && flags & CFselfrel)
        {       /* if we know it's relative address     */
                ad = s->Soffset - OFFSET() - 4;
                goto L1;
        }
        else
#endif
        {
                assert(TARGET_SEGMENTED || !tyfarfunc(s->ty()));
                FLUSH();
                reftoident(cseg,offset,s,val,flags);
        }
        break;

    case FLblock:                       /* displacement to another block */
        ad = uev->Vblock->Boffset - OFFSET() - 4;
        //printf("FLblock: funcoffset = %x, OFFSET = %x, Boffset = %x, ad = %x\n", funcoffset, OFFSET(), uev->Vblock->Boffset, ad);
        goto L1;

    case FLblockoff:
        FLUSH();
        assert(uev->Vblock);
        //printf("FLblockoff: offset = %x, Boffset = %x, funcoffset = %x\n", offset, uev->Vblock->Boffset, funcoffset);
        reftocodseg(cseg,offset,uev->Vblock->Boffset);
        break;

    default:
#ifdef DEBUG
        WRFL(fl);
#endif
        assert(0);
  }
  offset += 4;
}


STATIC void do16bit(enum FL fl,union evc *uev,int flags)
{ char *p;
  symbol *s;
  targ_size_t ad;

  switch (fl)
  {
    case FLconst:
        GENP(2,(char *) uev);
        return;
    case FLdatseg:
        FLUSH();
        reftodatseg(cseg,offset,uev->_EP.Vpointer,uev->_EP.Vseg,flags);
        break;
    case FLswitch:
        FLUSH();
        ad = uev->Vswitch->Btableoffset;
        if (config.flags & CFGromable)
                reftocodseg(cseg,offset,ad);
        else
                reftodatseg(cseg,offset,ad,JMPSEG,CFoff);
        break;
#if TARGET_SEGMENTED
    case FLcsdata:
    case FLfardata:
#endif
    case FLextern:                      /* external data symbol         */
    case FLtlsdata:
        assert(SIXTEENBIT || TARGET_SEGMENTED);
        FLUSH();
        s = uev->sp.Vsym;               /* symbol pointer               */
        reftoident(cseg,offset,s,uev->sp.Voffset,flags);
        break;
    case FLfunc:                        /* function call                */
        assert(SIXTEENBIT || TARGET_SEGMENTED);
        s = uev->sp.Vsym;               /* symbol pointer               */
        if (tyfarfunc(s->ty()))
        {       /* Large code references are always absolute    */
                FLUSH();
                offset += reftoident(cseg,offset,s,0,flags) - 2;
        }
        else if (s->Sseg == cseg &&
                 (s->Sclass == SCstatic || s->Sclass == SCglobal) &&
                 s->Sxtrnnum == 0 && flags & CFselfrel)
        {       /* if we know it's relative address     */
                ad = s->Soffset - OFFSET() - 2;
                goto L1;
        }
        else
        {       FLUSH();
                reftoident(cseg,offset,s,0,flags);
        }
        break;
    case FLblock:                       /* displacement to another block */
        ad = uev->Vblock->Boffset - OFFSET() - 2;
#ifdef DEBUG
        {
            targ_ptrdiff_t delta = uev->Vblock->Boffset - OFFSET() - 2;
            assert((signed short)delta == delta);
        }
#endif
    L1:
        GENP(2,&ad);                    // displacement
        return;

    case FLblockoff:
        FLUSH();
        reftocodseg(cseg,offset,uev->Vblock->Boffset);
        break;

    default:
#ifdef DEBUG
        WRFL(fl);
#endif
        assert(0);
  }
  offset += 2;
}

STATIC void do8bit(enum FL fl,union evc *uev)
{ char c;
  targ_ptrdiff_t delta;

  switch (fl)
  {
    case FLconst:
        c = uev->Vuns;
        break;
    case FLblock:
        delta = uev->Vblock->Boffset - OFFSET() - 1;
        if ((signed char)delta != delta)
        {
#if MARS
            if (uev->Vblock->Bsrcpos.Slinnum)
                fprintf(stderr, "%s(%d): ", uev->Vblock->Bsrcpos.Sfilename, uev->Vblock->Bsrcpos.Slinnum);
#endif
            fprintf(stderr, "block displacement of %lld exceeds the maximum offset of -128 to 127.\n", (long long)delta);
            err_exit();
        }
        c = delta;
#ifdef DEBUG
        assert(uev->Vblock->Boffset > OFFSET() || c != 0x7F);
#endif
        break;
    default:
#ifdef DEBUG
        fprintf(stderr,"fl = %d\n",fl);
#endif
        assert(0);
  }
  GEN(c);
}


/**********************************
 */

#if HYDRATE
void code_hydrate(code **pc)
{
    code *c;
    unsigned char ins,rm;
    enum FL fl;

    assert(pc);
    while (*pc)
    {
        c = (code *) ph_hydrate(pc);
        if (c->Iflags & CFvex)
            ins = vex_inssize(c);
        else if ((c->Iop & 0xFFFD00) == 0x0F3800)
            ins = inssize2[(c->Iop >> 8) & 0xFF];
        else if ((c->Iop & 0xFF00) == 0x0F00)
            ins = inssize2[c->Iop & 0xFF];
        else
            ins = inssize[c->Iop & 0xFF];
        switch (c->Iop)
        {
            default:
                break;

            case ESCAPE | ESClinnum:
                srcpos_hydrate(&c->IEV1.Vsrcpos);
                goto done;

            case ESCAPE | ESCctor:
            case ESCAPE | ESCdtor:
                el_hydrate(&c->IEV1.Vtor);
                goto done;

            case ASM:
                ph_hydrate(&c->IEV1.as.bytes);
                goto done;
        }
        if (!(ins & M) ||
            ((rm = c->Irm) & 0xC0) == 0xC0)
            goto do2;           /* if no first operand          */
        if (is32bitaddr(I32,c->Iflags))
        {

            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 4 && (c->Isib & 7) == 5 || (rm & 7) == 5))
               )
                goto do2;       /* if no first operand  */
        }
        else
        {
            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 6))
               )
                goto do2;       /* if no first operand  */
        }
        fl = (enum FL) c->IFL1;
        switch (fl)
        {
            case FLudata:
            case FLdata:
            case FLreg:
            case FLauto:
            case FLbprel:
            case FLpara:
#if TARGET_SEGMENTED
            case FLcsdata:
            case FLfardata:
#endif
            case FLtlsdata:
            case FLfunc:
            case FLpseudo:
            case FLextern:
            case FLtmp:
                assert(flinsymtab[fl]);
                symbol_hydrate(&c->IEVsym1);
                symbol_debug(c->IEVsym1);
                break;
            case FLdatseg:
            case FLfltreg:
            case FLallocatmp:
            case FLcs:
            case FLndp:
            case FLoffset:
            case FLlocalsize:
            case FLconst:
            case FLframehandler:
                assert(!flinsymtab[fl]);
                break;
            case FLcode:
                (void) ph_hydrate(&c->IEV1.Vcode);
                break;
            case FLblock:
            case FLblockoff:
                (void) ph_hydrate(&c->IEV1.Vblock);
                break;
#if SCPP
            case FLctor:
            case FLdtor:
                el_hydrate(&c->IEV1.Vtor);
                break;
#endif
            case FLasm:
                (void) ph_hydrate(&c->IEV1.as.bytes);
                break;
            default:
#ifdef DEBUG
                WRFL(fl);
#endif
                assert(0);
                break;
        }
    do2:
        /* Ignore TEST (F6 and F7) opcodes      */
        if (!(ins & T))
            goto done;          /* if no second operand */

        fl = (enum FL) c->IFL2;
        switch (fl)
        {
            case FLudata:
            case FLdata:
            case FLreg:
            case FLauto:
            case FLbprel:
            case FLpara:
#if TARGET_SEGMENTED
            case FLcsdata:
            case FLfardata:
#endif
            case FLtlsdata:
            case FLfunc:
            case FLpseudo:
            case FLextern:
            case FLtmp:
                assert(flinsymtab[fl]);
                symbol_hydrate(&c->IEVsym2);
                symbol_debug(c->IEVsym2);
                break;
            case FLdatseg:
            case FLfltreg:
            case FLallocatmp:
            case FLcs:
            case FLndp:
            case FLoffset:
            case FLlocalsize:
            case FLconst:
            case FLframehandler:
                assert(!flinsymtab[fl]);
                break;
            case FLcode:
                (void) ph_hydrate(&c->IEV2.Vcode);
                break;
            case FLblock:
            case FLblockoff:
                (void) ph_hydrate(&c->IEV2.Vblock);
                break;
            default:
#ifdef DEBUG
                WRFL(fl);
#endif
                assert(0);
                break;
        }
  done:
        ;

        pc = &code_next(c);
    }
}
#endif

/**********************************
 */

#if DEHYDRATE
void code_dehydrate(code **pc)
{
    code *c;
    unsigned char ins,rm;
    enum FL fl;

    while ((c = *pc) != NULL)
    {
        ph_dehydrate(pc);

        if (c->Iflags & CFvex)
            ins = vex_inssize(c);
        else if ((c->Iop & 0xFFFD00) == 0x0F3800)
            ins = inssize2[(c->Iop >> 8) & 0xFF];
        else if ((c->Iop & 0xFF00) == 0x0F00)
            ins = inssize2[c->Iop & 0xFF];
        else
            ins = inssize[c->Iop & 0xFF];
        switch (c->Iop)
        {
            default:
                break;

            case ESCAPE | ESClinnum:
                srcpos_dehydrate(&c->IEV1.Vsrcpos);
                goto done;

            case ESCAPE | ESCctor:
            case ESCAPE | ESCdtor:
                el_dehydrate(&c->IEV1.Vtor);
                goto done;

            case ASM:
                ph_dehydrate(&c->IEV1.as.bytes);
                goto done;
        }

        if (!(ins & M) ||
            ((rm = c->Irm) & 0xC0) == 0xC0)
            goto do2;           /* if no first operand          */
        if (is32bitaddr(I32,c->Iflags))
        {

            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 4 && (c->Isib & 7) == 5 || (rm & 7) == 5))
               )
                goto do2;       /* if no first operand  */
        }
        else
        {
            if (
                ((rm & 0xC0) == 0 && !((rm & 7) == 6))
               )
                goto do2;       /* if no first operand  */
        }
        fl = (enum FL) c->IFL1;
        switch (fl)
        {
            case FLudata:
            case FLdata:
            case FLreg:
            case FLauto:
            case FLbprel:
            case FLpara:
#if TARGET_SEGMENTED
            case FLcsdata:
            case FLfardata:
#endif
            case FLtlsdata:
            case FLfunc:
            case FLpseudo:
            case FLextern:
            case FLtmp:
                assert(flinsymtab[fl]);
                symbol_dehydrate(&c->IEVsym1);
                break;
            case FLdatseg:
            case FLfltreg:
            case FLallocatmp:
            case FLcs:
            case FLndp:
            case FLoffset:
            case FLlocalsize:
            case FLconst:
            case FLframehandler:
                assert(!flinsymtab[fl]);
                break;
            case FLcode:
                ph_dehydrate(&c->IEV1.Vcode);
                break;
            case FLblock:
            case FLblockoff:
                ph_dehydrate(&c->IEV1.Vblock);
                break;
#if SCPP
            case FLctor:
            case FLdtor:
                el_dehydrate(&c->IEV1.Vtor);
                break;
#endif
            case FLasm:
                ph_dehydrate(&c->IEV1.as.bytes);
                break;
            default:
#ifdef DEBUG
                WRFL(fl);
#endif
                assert(0);
                break;
        }
    do2:
        /* Ignore TEST (F6 and F7) opcodes      */
        if (!(ins & T))
            goto done;          /* if no second operand */

        fl = (enum FL) c->IFL2;
        switch (fl)
        {
            case FLudata:
            case FLdata:
            case FLreg:
            case FLauto:
            case FLbprel:
            case FLpara:
#if TARGET_SEGMENTED
            case FLcsdata:
            case FLfardata:
#endif
            case FLtlsdata:
            case FLfunc:
            case FLpseudo:
            case FLextern:
            case FLtmp:
                assert(flinsymtab[fl]);
                symbol_dehydrate(&c->IEVsym2);
                break;
            case FLdatseg:
            case FLfltreg:
            case FLallocatmp:
            case FLcs:
            case FLndp:
            case FLoffset:
            case FLlocalsize:
            case FLconst:
            case FLframehandler:
                assert(!flinsymtab[fl]);
                break;
            case FLcode:
                ph_dehydrate(&c->IEV2.Vcode);
                break;
            case FLblock:
            case FLblockoff:
                ph_dehydrate(&c->IEV2.Vblock);
                break;
            default:
#ifdef DEBUG
                WRFL(fl);
#endif
                assert(0);
                break;
        }
  done:
        ;
        pc = &code_next(c);
    }
}
#endif

/***************************
 * Debug code to dump code stucture.
 */

#if DEBUG

void WRcodlst(code *c)
{ for (; c; c = code_next(c))
        c->print();
}

void code::print()
{
    unsigned char ins;
    unsigned char rexb;
    code *c = this;

    if (c == CNIL)
    {   printf("code 0\n");
        return;
    }

    unsigned op = c->Iop;
    if (c->Iflags & CFvex)
        ins = vex_inssize(c);
    else if ((c->Iop & 0xFFFD00) == 0x0F3800)
        ins = inssize2[(op >> 8) & 0xFF];
    else if ((c->Iop & 0xFF00) == 0x0F00)
        ins = inssize2[op & 0xFF];
    else
        ins = inssize[op & 0xFF];

    printf("code %p: nxt=%p ",c,code_next(c));

    if (c->Iflags & CFvex)
    {
        if (c->Iflags & CFvex3)
        {   printf("vex=0xC4");
            printf(" 0x%02X", VEX3_B1(c->Ivex));
            printf(" 0x%02X", VEX3_B2(c->Ivex));
            rexb =
                ( c->Ivex.w ? REX_W : 0) |
                (!c->Ivex.r ? REX_R : 0) |
                (!c->Ivex.x ? REX_X : 0) |
                (!c->Ivex.b ? REX_B : 0);
        }
        else
        {   printf("vex=0xC5");
            printf(" 0x%02X", VEX2_B1(c->Ivex));
            rexb = !c->Ivex.r ? REX_R : 0;
        }
        printf(" ");
    }
    else
        rexb = c->Irex;

    if (rexb)
    {   printf("rex=0x%02X ", c->Irex);
        if (rexb & REX_W)
            printf("W");
        if (rexb & REX_R)
            printf("R");
        if (rexb & REX_X)
            printf("X");
        if (rexb & REX_B)
            printf("B");
        printf(" ");
    }
    printf("op=0x%02X",op);

  if ((op & 0xFF) == ESCAPE)
  {     if ((op & 0xFF00) == ESClinnum)
        {   printf(" linnum = %d\n",c->IEV1.Vsrcpos.Slinnum);
            return;
        }
        printf(" ESCAPE %d",c->Iop >> 8);
  }
  if (c->Iflags)
        printf(" flg=%x",c->Iflags);
  if (ins & M)
  {     unsigned rm = c->Irm;
        printf(" rm=0x%02X=%d,%d,%d",rm,(rm>>6)&3,(rm>>3)&7,rm&7);
        if (!I16 && issib(rm))
        {   unsigned char sib = c->Isib;
            printf(" sib=%02x=%d,%d,%d",sib,(sib>>6)&3,(sib>>3)&7,sib&7);
        }
        if ((rm & 0xC7) == BPRM || (rm & 0xC0) == 0x80 || (rm & 0xC0) == 0x40)
        {
            switch (c->IFL1)
            {
                case FLconst:
                case FLoffset:
                    printf(" int = %4d",c->IEV1.Vuns);
                    break;
                case FLblock:
                    printf(" block = %p",c->IEV1.Vblock);
                    break;
                case FLswitch:
                case FLblockoff:
                case FLlocalsize:
                case FLframehandler:
                case 0:
                    break;
                case FLdatseg:
                    printf(" %d.%llx",c->IEVseg1,(unsigned long long)c->IEVpointer1);
                    break;
                case FLauto:
                case FLreg:
                case FLdata:
                case FLudata:
                case FLpara:
                case FLtmp:
                case FLbprel:
                case FLtlsdata:
                    printf(" sym='%s'",c->IEVsym1->Sident);
                    break;
                case FLextern:
                    printf(" FLextern offset = %4d",(int)c->IEVoffset1);
                    break;
                default:
                    WRFL((enum FL)c->IFL1);
                    break;
            }
        }
  }
  if (ins & T)
  {     printf(" "); WRFL((enum FL)c->IFL2);
        switch (c->IFL2)
        {
            case FLconst:
                printf(" int = %4d",c->IEV2.Vuns);
                break;
            case FLblock:
                printf(" block = %p",c->IEV2.Vblock);
                break;
            case FLswitch:
            case FLblockoff:
            case 0:
            case FLlocalsize:
            case FLframehandler:
                break;
            case FLdatseg:
                printf(" %d.%llx",c->IEVseg2,(unsigned long long)c->IEVpointer2);
                break;
            case FLauto:
            case FLreg:
            case FLpara:
            case FLtmp:
            case FLbprel:
            case FLfunc:
            case FLdata:
            case FLudata:
            case FLtlsdata:
                printf(" sym='%s'",c->IEVsym2->Sident);
                break;
            case FLcode:
                printf(" code = %p",c->IEV2.Vcode);
                break;
            default:
                WRFL((enum FL)c->IFL2);
                break;
        }
  }
  printf("\n");
}
#endif

#endif // !SPP
