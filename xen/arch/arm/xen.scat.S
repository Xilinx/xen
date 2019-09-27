#define PAGE_SIZE 4096
#define POINTER_ALIGN 8
#define SMP_CACHE_BYTES 128
#define STACK_SIZE 32768
#define XEN_VIRT_START 0x00200000

LOAD XEN_VIRT_START
{
;_start
;_stext
  _text AlignExpr(+0, PAGE_SIZE) PADVALUE 0x9090
  {
    *(.text*)
    *(.text.cold)
    *(.text.unlikely)
    *(.fixup)
    *(.gnu.warning)
  }
;_etext

;_srodata
;__start_bug_frames
  _rodata_bug_frames_0 AlignExpr(+0, PAGE_SIZE)
  {
    *(.bug_frames.0)
  }
;__stop_bug_frames_0

  _rodata_bug_frames_1 +0 FIXED ZEROPAD
  {
    *(.bug_frames.1)
  }
;__stop_bug_frames_1

  _rodata_bug_frames_2 +0
  {
    *(.bug_frames.2)
  }
;__stop_bug_frames_2

  _rodata_data +0
  {
    *(.rodata)
    *(.rodata.*)
    *(.data.rel.ro)
    *(.data.rel.ro.*)
  }

#ifdef CONFIG_LOCK_PROFILE
;__lock_profile_start
  _rodata_lockprofile_data AlignExpr(+0, POINTER_ALIGN)
  {
    *(.lockprofile.data)
  }
;__lock_profile_end
#endif

;__param_start
  _rodata_data_param AlignExpr(+0, POINTER_ALIGN)
  {
    *(.data.param)
  }
;__param_end

;__proc_info_start
  _rodata_proc_info +0
  {
    *(.proc.info)
  }
;__proc_info_end

#if defined(CONFIG_HAS_VPCI) && defined(CONFIG_LATE_HWDOM)
;__start_vpci_array
  _rodata_data_vpci AlignExpr(+0, POINTER_ALIGN) SORTTYPE Lexical
  {
    *(SORT(.data.vpci.*))
  }
;__end_vpci_array
#endif

#if defined(BUILD_ID)
;__note_gnu_build_id_start
  _note_gnu_build_id +0
  {
    *(.note.gnu.build-id)
  }
;__note_gnu_build_id_end
#endif

;_erodata

  _data AlignExpr(+0, PAGE_SIZE)
  {
    *(.data.page_aligned.*)
    *(.data.*)
  }

;__start_schedulers_array
  _data_schedulers AlignExpr(+0, 8) FIXED ZEROPAD
  {
    *(.data.schedulers)
  }
;__end_schedulers_array

  _data_rel +0
  {
    *(.data.rel)
    *(.data.rel.*)
;#CONSTRUCTORS ????
  }

;__start___ex_table  
  _data_ex_table AlignExpr(+0, SMP_CACHE_BYTES)
  {
    *(.ex_table)
  }
;__stop___ex_table

;__start___pre_ex_table
  _data_ex_table_pre +0
  {
    *(.ex_table.pre)
  }
;__stop___pre_ex_table

  _data_read_mostly +0
  {
    *(.data.read_mostly)
  }

;_splatform
  _arch_info AlignExpr(+0, 8) 
  {
     *(.arch.info)
  }
;_eplatform

;_sdevice
  _dev_info AlignExpr(+0, 8)
  {
    *(.dev.info)
  }
;_edevice

;_asdevice
  _adev_info AlignExpr(+0, 8)
  {
    *(.adev.info)
  }
;_aedevice

;__init_begin
;_sinittext
  _init_text AlignExpr(+0, PAGE_SIZE)
  {
    *(.init.text)
  }
;_einittext

  _init_rodata AlignExpr(+0, PAGE_SIZE) FIXED ZEROPAD
  {
    *(.init.rodata)
    *(.init.rodata.rel)
    *(.init.rodata.str*)
  }

;__setup_start
  _init_setup AlignExpr(+0, POINTER_ALIGN)
  {
    *(.init.setup)
  }
;__setup_end

;__initcall_start
  _initcallpresmp_init +0
  {
    *(.initcallpresmp.init)
  }
;__presmp_initcall_end

  _initcall1_init +0
  {
    *(.initcall1.init)
  }
;__initcall_end

;__alt_instructions
  _altinstructions AlignExpr(+0, 4)
  {
    *(.altinstructions)
  }
;__alt_instructions_end

  _altinstr_replacement AlignExpr(+0, 4)
  {
    *(.altinstr_replacement)
  }

  _init_data +0
  {
    *(.init.data)
    *(.init.data.rel)
    *(.init.data.rel.*)
  }

;__ctors_start
  _ctors AlignExpr(+0, 8)
  {
    *(.ctors)
    *(.init_array)
  }

  _init_array_sorted AlignExpr(+0, 8) SORTTYPE Lexical
  {
    *(.init_array.*)
  }
;__ctors_end

#if defined(CONFIG_HAS_VPCI) && !defined(CONFIG_LATE_HWDOM)
  _data_vpci AlignExpr(+0, POINTER_ALIGN) SORTTYPE Lexical
  {
    *(.data.vpci.*)
  }
#endif
;__init_end_efi

;__init_end
;__bss_start
  _bss AlignExpr(+0, STACK_SIZE)
  {
    *(.bss.stack_aligned*)
    *(.bss.page_aligned*, OVERALIGN PAGE_SIZE)
    *(.bss*)
  }


;__per_cpu_start
  _bss_percpu AlignExpr(+0, SMP_CACHE_BYTES)
  {
    *(.bss.percpu)
    *(.bss.percpu.read_mostly, OVERALIGN SMP_CACHE_BYTES)
  }
;__per_cpu_data_end
;__bss_end
;_end

#ifdef CONFIG_DTB_FILE
;_sdtb
  _dtb
 {
    *(.dtb)
 }
#endif

}


