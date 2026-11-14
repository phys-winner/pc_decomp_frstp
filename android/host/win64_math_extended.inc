/* Win64 long is 32 bits on both hosts; pointer outputs never use native long.
 * Transcendentals use typed libm with a measured ULP bound, preserving native FP/errno. */
static uint64_t extended_transcend(uint64_t bits,unsigned kind,unsigned rounding,unsigned *flags){
    static double (*const functions[])(double)={exp,atan,cosh,sinh,tanh};
    double input,output;uint64_t result;int saved_errno=errno;unsigned raised,wide=kind<2||kind==5;
    memcpy(&input,&bits,8);
#if defined(__aarch64__)
    uint64_t control,status,actual;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,wide?rounding:0);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#else
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(wide?rounding<<13:0));
#endif
    if(kind<5){double (*volatile function)(double)=functions[kind];output=function(input);}
    else{
#if defined(__ANDROID__)
        int sign;output=lgamma_r(input,&sign);
#else
        double (*volatile function)(double)=lgamma;output=function(input);
#endif
    }
    if(wide)memcpy(&result,&output,8);
    else{
        float narrowed;
#if defined(__aarch64__)
        uint64_t conversion=native_control(control,rounding);
        __asm__ volatile("msr fpcr,%[mode]\n\tfcvt %s[out],%d[in]"
            :[out]"=w"(narrowed):[mode]"r"(conversion),[in]"w"(output):"memory");
#else
        unsigned status=_mm_getcsr()&63u;_mm_setcsr(0x1f80u|(rounding<<13)|status);
        narrowed=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(output)));
#endif
        uint32_t low;memcpy(&low,&narrowed,4);result=low;
    }
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpsr\n\tmsr fpcr,%1\n\tmsr fpsr,%2":"=&r"(actual):"r"(control),"r"(status):"memory");
    raised=((actual&1)?1:0)|((actual&2)?4:0)|((actual&4)?8:0)|((actual&8)?16:0)|((actual&16)?32:0);
#else
    raised=_mm_getcsr()&63u;_mm_setcsr(control);
#endif
    errno=saved_errno;*flags|=raised;return result;
}

static int extended_pointer_math(CPU *c,const char *name){
    unsigned split=!strcmp(name,"modf");if(!split&&strcmp(name,"frexp"))return 0;
    uint64_t input=c->xmm[0].u64[0],sign=input&UINT64_C(0x8000000000000000),m=input&~sign;
    uint64_t fraction=input,integral=input;unsigned flags=0;
    const uint64_t inf=UINT64_C(0x7ff0000000000000),quiet=UINT64_C(0x8000000000000);
    if(split){
        if(m>inf){fraction|=quiet;if(!(m&quiet))flags=1;}
        else if(m>=UINT64_C(0x4340000000000000))fraction=sign;
        else if(m<UINT64_C(0x3ff0000000000000))integral=sign;
        else{
            unsigned shift=1075u-(unsigned)(m>>52);uint64_t tail=m&((UINT64_C(1)<<shift)-1);
            integral=input&~((UINT64_C(1)<<shift)-1);
            if(!tail)fraction=sign|(((c->mxcsr>>13)&3u)==1?UINT64_C(0x8000000000000000):0);
            else{
                unsigned exponent=(unsigned)(m>>52);
                while(!(tail&(UINT64_C(1)<<52))){tail<<=1;--exponent;}
                fraction=sign|((uint64_t)exponent<<52)|(tail&UINT64_C(0xfffffffffffff));
            }
        }
        memcpy(win64_pointer(c->rdx,8,1),&integral,8);
    }else{
        int exponent=0;
        if(m>=inf){
            exponent=-1;
            if(m>inf){fraction|=quiet;if(!(m&quiet)){flags=1;win64_stdio_set_errno(33);}}
        }else if(m){
            uint64_t mantissa=m;unsigned e=(unsigned)(m>>52);
            if(!e){
                if(c->mxcsr&64u)goto frexp_write;
                flags=2;e=1;
                while(!(mantissa&(UINT64_C(1)<<52))){mantissa<<=1;--exponent;}
            }
            exponent+=(int)e-1022;
            fraction=sign|UINT64_C(0x3fe0000000000000)|(mantissa&UINT64_C(0xfffffffffffff));
        }
frexp_write:
        memcpy(win64_pointer(c->rdx,4,1),&exponent,4);
    }
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;c->xmm[0].u64[0]=fraction;return 1;
}

static int extended_integer_math(CPU *c,const char *name){
    unsigned ties_away=!strcmp(name,"lroundf");if(!ties_away&&strcmp(name,"lrintf"))return 0;
    uint32_t input=c->xmm[0].u32[0],m=input&UINT32_C(0x7fffffff),result=0;
    unsigned negative=input>>31,rounding=(c->mxcsr>>13)&3u,flags=0;
    if(m>UINT32_C(0x4f000000)){flags=1;win64_stdio_set_errno(33);}
    else if(m==UINT32_C(0x4f000000)){result=UINT32_C(0x80000000);flags=negative?0:1;}
    else if(m&&!(m<UINT32_C(0x800000)&&!ties_away&&(c->mxcsr&64u))){
        int exponent=(int)(m>>23)-127;uint32_t tail=0,half=0;
        if(exponent<0){tail=m;half=UINT32_C(0x3f000000);}
        else if(exponent<23){unsigned shift=23u-(unsigned)exponent;uint32_t mantissa=(m&UINT32_C(0x7fffff))|UINT32_C(0x800000);
            result=mantissa>>shift;tail=mantissa&((1u<<shift)-1u);half=1u<<(shift-1);}
        else result=((m&UINT32_C(0x7fffff))|UINT32_C(0x800000))<<(exponent-23);
        if(tail){
            if(ties_away?tail>=half:rounding==0?(tail>half||(tail==half&&(result&1))):rounding==1?negative:rounding==2?!negative:0)++result;
            if(!ties_away)flags=32;
        }
        if(negative)result=0u-result;
    }
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;SET32(c->rax,result);return 1;
}

static int extended_math_call(CPU *c,const char *name){
    if(extended_pointer_math(c,name)||extended_integer_math(c,name))return 1;
    if(!strcmp(name,"copysign")){
        c->xmm[0].u64[0]=(c->xmm[0].u64[0]&UINT64_C(0x7fffffffffffffff))|(c->xmm[1].u64[0]&UINT64_C(0x8000000000000000));return 1;
    }
    unsigned kind;
    if(!strcmp(name,"exp"))kind=0;else if(!strcmp(name,"atan"))kind=1;
    else if(!strcmp(name,"coshf"))kind=2;else if(!strcmp(name,"sinhf"))kind=3;
    else if(!strcmp(name,"tanhf"))kind=4;else if(!strcmp(name,"lgamma")||!strcmp(name,"lgammal"))kind=5;
    else if(!strcmp(name,"lgammaf"))kind=6;else return 0;
    unsigned wide=kind<2||kind==5;
    uint64_t input=wide?c->xmm[0].u64[0]:c->xmm[0].u32[0];
    uint64_t sign=input&(wide?UINT64_C(0x8000000000000000):UINT64_C(0x80000000)),m=input&~sign,result=input;
    uint64_t inf=wide?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000);
    uint64_t quiet=wide?UINT64_C(0x8000000000000):UINT64_C(0x400000);
    unsigned flags=0,rounding=(c->mxcsr>>13)&3u;
    if(m>inf){result=kind>=5?input:input|quiet;if(kind==2||kind==3)flags=(m&quiet)?0:1;}
    else if(kind>=5){
        if(m==inf)result=inf;
        else if(!m||(m<(wide?UINT64_C(0x10000000000000):UINT64_C(0x800000))&&(c->mxcsr&64u))){result=inf;flags=m?36:4;win64_stdio_set_errno(34);}
        else{
            result=extended_transcend(wide?input:angle_double((uint32_t)input),kind,rounding,&flags);
            if(flags&8)result=inf;
            if(flags&1)win64_stdio_set_errno(33);else if(flags&12)win64_stdio_set_errno(34);
        }
    }
    else if(kind==4){
        if(m>UINT32_C(0x41200000))result=sign|UINT32_C(0x3f800000);
        else if(m){flags=32;if(m>=UINT32_C(0x38800000)){unsigned ignored=0;result=extended_transcend(angle_double((uint32_t)input),kind,rounding,&ignored);}}
    }else if(kind>=2){
        if(m==inf)result=kind==2?inf:input;
        else if(!m||(m<UINT32_C(0x800000)&&(c->mxcsr&64u)))result=kind==2?UINT32_C(0x3f800000):input;
        else if(m>UINT32_C(0x42b2d4fc)){result=(kind==2?0:sign)|inf;flags=kind==2?40:8;win64_stdio_set_errno(34);}
        else if(m<UINT32_C(0x38800000)){result=kind==2?UINT32_C(0x3f800000):input;flags=32|(m<UINT32_C(0x800000)?2:0);}
        else{result=extended_transcend(angle_double((uint32_t)input),kind,rounding,&flags);flags=32;}
    }else{
        if(m==inf){result=kind?(sign|UINT64_C(0x3ff921fb54442d18)):(sign?0:inf);flags=kind?32:0;}
        else if(!m||(m<UINT64_C(0x10000000000000)&&(c->mxcsr&64u)))result=kind?sign:UINT64_C(0x3ff0000000000000);
        else{
            result=extended_transcend(input,kind,rounding,&flags);
            if(m<UINT64_C(0x10000000000000))flags|=2;
            if(!kind){
                if((flags&8u)&&!sign)win64_stdio_set_errno(34);
                if(sign&&m>UINT64_C(0x4087500000000000)){result=rounding==2?1:0;flags=0;}
                if(c->mxcsr&(32768u|64u)){if((result&UINT64_C(0x7ff0000000000000))==0)result=0;}
            }else if(c->mxcsr&32768u){if((result&UINT64_C(0x7ff0000000000000))==0)result=sign;}
        }
    }
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;
    if(wide)c->xmm[0].u64[0]=result;else c->xmm[0].u32[0]=(uint32_t)result;
    return 1;
}
