.code
steam_abi_return PROC
    mov rax,123456789abcdef0h
    mov QWORD PTR [rdx],rax
    mov rax,rdx
    ret
steam_abi_return ENDP
game_abi_return PROC
    mov rax,0fedcba9876543210h
    mov QWORD PTR [rdx],rax
    mov rax,rdx
    ret
game_abi_return ENDP
steam_abi_values PROC
    mov QWORD PTR [rcx+8],rdx
    mov rax,QWORD PTR [r8]
    mov QWORD PTR [rcx+16],rax
    mov QWORD PTR [rcx+24],r9
    mov rax,55aa55aa55aa55aah
    ret
steam_abi_values ENDP
END
