#include <xen/cache.h>
#include <asm/page.h>
#include <asm/percpu.h>
#undef ENTRY
#undef ALIGN

LOAD XEN_VIRT_START
{
  .text AlignExpr(+0, PAGE_SIZE) PADVALUE 0x9090
  {
    *(.text)
    *(.text.cold)
    *(.text.unlikely)
    *(.fixup)
    *(.gnu.warning)
  }

  .rodata.bug_frames.0 AlignExpr(+0, PAGE_SIZE)
  {
    *(.bug_frames.0)
  }

  .rodata.bug_frames.1 +0
  {
    *(.bug_frames.1)
  }

  .rodata.bug_frames.2 +0
  {
    *(.bug_frames.2)
  }

  .rodata.data +0
  {
    *(.rodata)
    *(.rodata.*)
    *(.data.rel.ro)
    *(.data.rel.ro.*)
  }

#ifdef CONFIG_LOCK_PROFILE
  .rodata.lockprofile.data AlignExpr(+0, POINTER_ALIGN)
  {
    *(.lockprofile.data)
  }
#endif

  .rodata.data.param AlignExpr(+0, POINTER_ALIGN)
  {
    *(.data.param)
  }

  .rodata.proc.info +0
  {
    *(.proc.info)
  }

#if defined(CONFIG_HAS_VPCI) && defined(CONFIG_LATE_HWDOM)
  .rodata.data.vpci AlignExpr(+0, POINTER_ALIGN)
  {
    *(SORT(.data.vpci.*))
  }
#endif

#if defined(BUILD_ID)
  .note.gnu.build-id +0
  {
    *(.note.gnu.build-id)
  }
#endif

  .data AlignExpr(+0, PAGE_SIZE)
  {
    *(.data.page_aligned)
    *(.data)
  }

  .data.schedulers AlignExpr(+0, 8)
  {
    *(.data.schedulers)
  }

  .data.rel +0
  {
    *(.data.rel)
    *(.data.rel.*)
;#CONSTRUCTORS ????
  }

  .data.ex_table AlignExpr(+0, SMP_CACHE_BYTES)
  {
    *(.ex_table)
  }

  .data.ex_table.pre +0
  {
    *(.ex_table.pre)
  }

  .data.read_mostly +0
  {
    *(.data.read_mostly)
  }

  .dev.info AlignExpr(+0, 8)
  {
    *(.dev.info)
  }

  .adev.info AlignExpr(+0, 8)
  {
    *(.adev.info)
  }

  .init.text AlignExpr(+0, PAGE_SIZE)
  {
    *(.init.text)
  }

  .init.rodata AlignExpr(+0, PAGE_SIZE)
  {
    *(.init.rodata)
    *(.init.rodata.rel)
    *(.init.rodata.str*)
  }

  .init.setup AlignExpr(+0, POINTER_ALIGN)
  {
    *(.init.setup)
  }

  .initcallpresmp.init +0
  {
    *(.initcallpresmp.init)
  }

  .initcall1.init +0
  {
    *(.initcall1.init)
  }

  .altinstructions AlignExpr(+0, 4)
  {
    *(.altinstructions)
  }

  .altinstr_replacement AlignExpr(+0, 4)
  {
    *(.altinstr_replacement)
  }

  .init.data +0
  {
    *(.init.data)
    *(.init.data.rel)
    *(.init.data.rel.*)
  }

  .ctors AlignExpr(+0, 8)
  {
    *(.ctors)
    *(.init_array)
    *(SORT(.init_array.*))
  }

#if defined(CONFIG_HAS_VPCI) && !defined(CONFIG_LATE_HWDOM)
  .data.vpci AlignExpr(+0, POINTER_ALIGN)
  {
    *(SORT(.data.vpci.*))
  }
#endif

  .bss AlignExpr(+0, STACK_SIZE)
  {
    *(.bss.stack_aligned)
    *(.bss.page_aligned, OVERALIGN PAGE_SIZE)
    *(.bss)
  }

  .bss.percpu AlignExpr(+0, SMP_CACHE_BYTES)
  {
    *(.bss.percpu)
    *(.bss.percpu.read_mostly, OVERALIGN SMP_CACHE_BYTES)
  }
}


