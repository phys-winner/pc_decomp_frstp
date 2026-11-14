#define UNICODE
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

typedef struct {int baseline,show,hide;DWORD show_error,hide_error;int show_errno,hide_errno;} thread_result;
static thread_result other;
static int one(BOOL value,DWORD *error,int *saved_errno){SetLastError(0x5a17);errno=1927;int r=ShowCursor(value);*error=GetLastError();*saved_errno=errno;return r;}
static DWORD WINAPI worker(void *unused){
    (void)unused;MSG msg;PeekMessageW(&msg,NULL,0,0,PM_NOREMOVE);
    other.show=one(TRUE,&other.show_error,&other.show_errno);
    other.hide=one(FALSE,&other.hide_error,&other.hide_errno);other.baseline=other.hide;
    return 0;
}
int main(void){
    MSG msg;PeekMessageW(&msg,NULL,0,0,PM_NOREMOVE);
    CURSORINFO cursor_before={0};cursor_before.cbSize=sizeof(cursor_before);BOOL cursor_before_ok=GetCursorInfo(&cursor_before);
    DWORD error;int e;int first=one(TRUE,&error,&e);int baseline=one(FALSE,&error,&e);if(first!=baseline+1)return 2;
    int mouse=GetSystemMetrics(SM_MOUSEPRESENT),current=baseline;
    BOOL values[]={FALSE,FALSE,TRUE,TRUE,FALSE,TRUE,TRUE,FALSE,FALSE,TRUE,2,FALSE,-1,FALSE,(BOOL)0x80000000u,FALSE};
    puts("{\"own_child_threads_only\":true,\"no_SetCursor_or_position_or_window_operations\":true,");
    printf("\"SM_MOUSEPRESENT\":%d,\"baseline\":%d,\"rows\":[\n",mouse,baseline);
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i){
        current=one(values[i],&error,&e);
        printf("%s{\"argument_low32_hex\":\"%08x\",\"counter\":%d,\"LastError\":%lu,\"errno\":%d}",i?",\n":"",(unsigned)(uint32_t)values[i],current,(unsigned long)error,e);
    }
    /* Restore the exact measured starting count even if a BOOL quirk occurs. */
    for(unsigned n=0;current!=baseline&&n<32;++n)current=one(current<baseline,&error,&e);
    if(current!=baseline)return 3;
    int held=one(FALSE,&error,&e);
    HANDLE t=CreateThread(NULL,0,worker,NULL,0,NULL);if(!t){one(TRUE,&error,&e);return 4;}
    DWORD wait=WaitForSingleObject(t,5000);CloseHandle(t);int restored=one(TRUE,&error,&e);if(wait!=WAIT_OBJECT_0)return 5;
    CURSORINFO cursor_after={0};cursor_after.cbSize=sizeof(cursor_after);BOOL cursor_after_ok=GetCursorInfo(&cursor_after);
    printf("\n],\"main_count_held_while_worker_probed\":%d,\"worker\":{\"baseline\":%d,\"show\":%d,\"hide\":%d,\"show_LastError\":%lu,\"hide_LastError\":%lu,\"show_errno\":%d,\"hide_errno\":%d},\"main_restored\":%d,\"balanced\":%s,",held,other.baseline,other.show,other.hide,(unsigned long)other.show_error,(unsigned long)other.hide_error,other.show_errno,other.hide_errno,restored,restored==baseline?"true":"false");
    printf("\"physical_cursor_before\":{\"query_BOOL\":%ld,\"flags\":%lu,\"HCURSOR_hex\":\"%016llx\",\"x\":%ld,\"y\":%ld},\"physical_cursor_after\":{\"query_BOOL\":%ld,\"flags\":%lu,\"HCURSOR_hex\":\"%016llx\",\"x\":%ld,\"y\":%ld}}\n",(long)cursor_before_ok,(unsigned long)cursor_before.flags,(unsigned long long)(uintptr_t)cursor_before.hCursor,(long)cursor_before.ptScreenPos.x,(long)cursor_before.ptScreenPos.y,(long)cursor_after_ok,(unsigned long)cursor_after.flags,(unsigned long long)(uintptr_t)cursor_after.hCursor,(long)cursor_after.ptScreenPos.x,(long)cursor_after.ptScreenPos.y);
    return restored==baseline?0:6;
}
