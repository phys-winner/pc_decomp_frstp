/* Values outside the specialized UCRT paths still have a real libm implementation.
 * This path keeps guest DAZ/FTZ and native thread state separate. */
static int portable_math_call(CPU *c,const char *name){
    static const char *names[]={"expf","exp2f","logf","log10f","atanf","fmodf","powf","atan2f","log","sin","cos","pow","ldexp"};
    unsigned kind=0;while(kind<sizeof(names)/sizeof(*names)&&strcmp(name,names[kind]))++kind;
    if(kind==sizeof(names)/sizeof(*names))return 0;
    unsigned wide=kind>=8,flags=0,rounding=(c->mxcsr>>13)&3u;
    uint64_t x=wide?c->xmm[0].u64[0]:c->xmm[0].u32[0],y=wide?c->xmm[1].u64[0]:c->xmm[1].u32[0];
    uint64_t sign=wide?UINT64_C(0x8000000000000000):UINT64_C(0x80000000),minimum=wide?UINT64_C(0x10000000000000):UINT64_C(0x800000);
    unsigned binary=kind==5||kind==6||kind==7||kind==11;
    if((x&~sign)&&(x&~sign)<minimum){if(c->mxcsr&64u)x&=sign;else flags|=2;}
    if(binary&&(y&~sign)&&(y&~sign)<minimum){if(c->mxcsr&64u)y&=sign;else flags|=2;}
    int saved_errno=errno,native_errno;uint64_t result=0;
#if defined(__aarch64__)
    uint64_t control,status,actual;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,(kind==9||kind==10)?0:rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#else
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|((kind==9||kind==10)?0:rounding<<13));
#endif
    errno=0;
    if(wide){
        double a,b,value;memcpy(&a,&x,8);memcpy(&b,&y,8);
        if(kind==8){double(*volatile fn)(double)=log;value=fn(a);}
        else if(kind==9){double(*volatile fn)(double)=sin;value=fn(a);}
        else if(kind==10){double(*volatile fn)(double)=cos;value=fn(a);}
        else if(kind==11){double(*volatile fn)(double,double)=pow;value=fn(a,b);}
        else{double(*volatile fn)(double,int)=ldexp;value=fn(a,(int32_t)c->rdx);}
        memcpy(&result,&value,8);
    }else{
        uint32_t low=(uint32_t)x,other=(uint32_t)y;float a,b,value;memcpy(&a,&low,4);memcpy(&b,&other,4);
        if(kind<=4){static float(*const functions[])(float)={expf,exp2f,logf,log10f,atanf};float(*volatile fn)(float)=functions[kind];value=fn(a);}
        else{static float(*const functions[])(float,float)={fmodf,powf,atan2f};float(*volatile fn)(float,float)=functions[kind-5];value=fn(a,b);}
        memcpy(&low,&value,4);result=low;
    }
    native_errno=errno;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpsr\n\tmsr fpcr,%1\n\tmsr fpsr,%2":"=&r"(actual):"r"(control),"r"(status):"memory");
    flags|=((actual&1)?1:0)|((actual&2)?4:0)|((actual&4)?8:0)|((actual&8)?16:0)|((actual&16)?32:0);
#else
    flags|=_mm_getcsr()&63u;_mm_setcsr(control);
#endif
    errno=saved_errno;
    uint64_t magnitude=result&~sign,inf=wide?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000);
    unsigned nan_input=(x&~sign)>inf||(binary&&(y&~sign)>inf);
    if(native_errno==EDOM||((flags&1)&&!nan_input))win64_stdio_set_errno(33);
    else if(native_errno==ERANGE||(flags&12))win64_stdio_set_errno(34);
    if((flags&1)&&magnitude>inf)result|=sign;
    if((c->mxcsr&32768u)&&magnitude&&magnitude<minimum){result&=sign;flags|=48;}
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;if(wide)c->xmm[0].u64[0]=result;else c->xmm[0].u32[0]=(uint32_t)result;return 1;
}
