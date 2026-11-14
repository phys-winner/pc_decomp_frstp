#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
static unsigned checks,invalid_calls;
static void invalid_handler(const wchar_t *expression,const wchar_t *function,const wchar_t *file,unsigned line,uintptr_t reserved){(void)expression;(void)function;(void)file;(void)line;(void)reserved;++invalid_calls;}
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"ENV_ORACLE_FAIL line=%d errno=%d\n",__LINE__,errno);return 2;}}while(0)
int main(void){CHECK(GetACP()==65001);_set_invalid_parameter_handler(invalid_handler);
    static const char *names[]={"FROST_ENV_CASE","frost_env_case","FrOsT_EnV_CaSe","FROST_ENV_EMPTY","FROST_ENV_EQUAL","FROST_ENV_UTF8","FROST_ENV_MISSING","","FROST_ENV_CASE=x","__ALSOFT_HALF_ANGLE_CONES"};
    static const char *expected[]={"CaseValue","CaseValue","CaseValue","","A=B","\xc3\xa9\xf0\x9f\x8c\xa8",NULL,NULL,NULL,NULL};
    char *stable=getenv(names[0]);CHECK(stable!=NULL);
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i){errno=1234;char *value=getenv(names[i]);CHECK((value!=NULL)==(expected[i]!=NULL));CHECK(!value||!strcmp(value,expected[i]));CHECK(errno==1234);CHECK(!strcmp(stable,"CaseValue"));}
    CHECK(getenv(names[0])==getenv(names[1]));
    static const wchar_t *wide_names[]={L"FROST_ENV_CASE",L"frost_env_case",L"FrOsT_EnV_CaSe",L"FROST_ENV_EMPTY",L"FROST_ENV_EQUAL",L"FROST_ENV_UTF8",L"FROST_ENV_MISSING",L"",L"FROST_ENV_CASE=x",L"__ALSOFT_HALF_ANGLE_CONES"};
    static const wchar_t *wide_expected[]={L"CaseValue",L"CaseValue",L"CaseValue",L"",L"A=B",L"\u00e9\U0001f328",NULL,NULL,NULL,NULL};
    wchar_t *wide_stable=_wgetenv(wide_names[0]);CHECK(wide_stable!=NULL);
    for(unsigned i=0;i<sizeof(wide_names)/sizeof(*wide_names);++i){errno=1234;wchar_t *value=_wgetenv(wide_names[i]);CHECK((value!=NULL)==(wide_expected[i]!=NULL));CHECK(!value||!wcscmp(value,wide_expected[i]));CHECK(errno==1234);CHECK(!wcscmp(wide_stable,L"CaseValue"));}
    CHECK(_wgetenv(wide_names[0])==_wgetenv(wide_names[1]));
    char query[256];for(unsigned mask=0;mask<256;++mask){strcpy_s(query,sizeof(query),names[0]);for(unsigned j=0;j<8;++j)if(mask&(1u<<j))query[j]=(char)(query[j]>='A'&&query[j]<='Z'?query[j]+32:query[j]);CHECK(getenv(query)==stable);}
    errno=0;CHECK(getenv(NULL)==NULL&&errno==EINVAL&&invalid_calls==1);
    errno=0;CHECK(_wgetenv(NULL)==NULL&&errno==EINVAL&&invalid_calls==2);
    static char long_name[32768];static wchar_t long_wide[32768];memset(long_name,'A',sizeof(long_name));long_name[32767]=0;for(unsigned i=0;i<32767;++i)long_wide[i]=L'A';
    errno=0;CHECK(getenv(long_name)==NULL&&errno==EINVAL&&invalid_calls==3);errno=0;CHECK(_wgetenv(long_wide)==NULL&&errno==EINVAL&&invalid_calls==4);
    printf("{\"success\":true,\"checks\":%u,\"active_codepage\":65001,\"empty_value_is_nonnull\":true,\"ASCII_case_insensitive\":true,\"stable_without_mutation\":true,\"invalid_parameter_callbacks\":%u,\"invalid_errno\":22}\n",checks,invalid_calls);return 0;}
