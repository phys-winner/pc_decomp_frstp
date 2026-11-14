.text
.global steam_abi_return
.type steam_abi_return,%function
steam_abi_return:
    movz x0,#0xdef0
    movk x0,#0x9abc,lsl #16
    movk x0,#0x5678,lsl #32
    movk x0,#0x1234,lsl #48
    ret
.global game_abi_return
.type game_abi_return,%function
game_abi_return:
    movz x0,#0x3210
    movk x0,#0x7654,lsl #16
    movk x0,#0xba98,lsl #32
    movk x0,#0xfedc,lsl #48
    str x0,[x8]
    ret
.global steam_abi_values
.type steam_abi_values,%function
steam_abi_values:
    ldr x2,[x2]
    stp x1,x2,[x0,#8]
    str x3,[x0,#24]
    movz x0,#0x55aa
    movk x0,#0x55aa,lsl #16
    movk x0,#0x55aa,lsl #32
    movk x0,#0x55aa,lsl #48
    ret
