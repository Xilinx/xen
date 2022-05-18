        .section .dtb,#alloc
        .align 12
        GLOBAL(_sdtb)
        .incbin CONFIG_DTB_FILE
