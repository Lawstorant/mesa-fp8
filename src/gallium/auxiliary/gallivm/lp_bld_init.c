/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#include "util/detect.h"
#include "util/compiler.h"
#include "util/macros.h"
#include "util/u_cpu_detect.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/os_time.h"
#include "lp_bld.h"
#include "lp_bld_debug.h"
#include "lp_bld_misc.h"
#include "lp_bld_init.h"
#include "lp_bld_coro.h"
#include "lp_bld_printf.h"

#include <llvm/Config/llvm-config.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

static bool gallivm_initialized = false;

/*
 * Optimization values are:
 * - 0: None (-O0)
 * - 1: Less (-O1)
 * - 2: Default (-O2, -Os)
 * - 3: Aggressive (-O3)
 *
 * See also CodeGenOpt::Level in llvm/Target/TargetMachine.h
 */
enum LLVM_CodeGenOpt_Level {
   None,        // -O0
   Less,        // -O1
   Default,     // -O2, -Os
   Aggressive   // -O3
};


/**
 * Create the LLVM (optimization) pass manager and install
 * relevant optimization passes.
 * \return  TRUE for success, FALSE for failure
 */
static bool
create_pass_manager(struct gallivm_state *gallivm)
{
   {
      char *td_str;
      // New ones from the Module.
      td_str = LLVMCopyStringRepOfTargetData(gallivm->target);
      LLVMSetDataLayout(gallivm->module, td_str);
      free(td_str);
   }

   return lp_passmgr_create(gallivm->module, &gallivm->passmgr);
}

/**
 * Free gallivm object's LLVM allocations, but not any generated code
 * nor the gallivm object itself.
 */
void
gallivm_free_ir(struct gallivm_state *gallivm)
{
   if (gallivm->passmgr)
      lp_passmgr_dispose(gallivm->passmgr);

   if (gallivm->engine) {
      /* This will already destroy any associated module.
       *Destroy the execution engine later if we need to keep debug info around.
       */
      if (!(gallivm_debug & GALLIVM_DEBUG_SYMBOLS))
         LLVMDisposeExecutionEngine(gallivm->engine);
   } else if (gallivm->module) {
      LLVMDisposeModule(gallivm->module);
   }

   if (gallivm->cache) {
      lp_free_objcache(gallivm->cache->jit_obj_cache);
      free(gallivm->cache->data);
   }
   FREE(gallivm->module_name);
   FREE(gallivm->file_name);

   if (gallivm->target) {
      LLVMDisposeTargetData(gallivm->target);
   }

   if (gallivm->builder)
      LLVMDisposeBuilder(gallivm->builder);

   if (gallivm->di_builder)
      LLVMDisposeDIBuilder(gallivm->di_builder);

   /* The LLVMContext should be owned by the parent of gallivm. */

   gallivm->engine = NULL;
   gallivm->target = NULL;
   gallivm->module = NULL;
   gallivm->module_name = NULL;
   gallivm->file_name = NULL;
   gallivm->passmgr = NULL;
   gallivm->context = NULL;
   gallivm->builder = NULL;
   gallivm->cache = NULL;
}


/**
 * Free LLVM-generated code.  Should be done AFTER gallivm_free_ir().
 */
static void
gallivm_free_code(struct gallivm_state *gallivm)
{
   assert(!gallivm->module);
   assert(!gallivm->engine);
   lp_free_generated_code(gallivm->code);
   gallivm->code = NULL;
   lp_free_memory_manager(gallivm->memorymgr);
   gallivm->memorymgr = NULL;
}


static bool
init_gallivm_engine(struct gallivm_state *gallivm)
{
   if (1) {
      enum LLVM_CodeGenOpt_Level optlevel;
      char *error = NULL;
      int ret;

      if (gallivm_perf & GALLIVM_PERF_NO_OPT) {
         optlevel = None;
      }
      else {
         optlevel = Default;
      }

      ret = lp_build_create_jit_compiler_for_module(&gallivm->engine,
                                                    &gallivm->code,
                                                    gallivm->cache,
                                                    gallivm->module,
                                                    gallivm->memorymgr,
                                                    (unsigned) optlevel,
                                                    &error);
      if (ret) {
         _debug_printf("%s\n", error);
         LLVMDisposeMessage(error);
         goto fail;
      }
   }

   if (0) {
       /*
        * Dump the data layout strings.
        */

       LLVMTargetDataRef target = LLVMGetExecutionEngineTargetData(gallivm->engine);
       char *data_layout;
       char *engine_data_layout;

       data_layout = LLVMCopyStringRepOfTargetData(gallivm->target);
       engine_data_layout = LLVMCopyStringRepOfTargetData(target);

       if (1) {
          debug_printf("module target data = %s\n", data_layout);
          debug_printf("engine target data = %s\n", engine_data_layout);
       }

       free(data_layout);
       free(engine_data_layout);
   }

   return true;

fail:
   return false;
}


/**
 * Allocate gallivm LLVM objects.
 * \return  TRUE for success, FALSE for failure
 */
static bool
init_gallivm_state(struct gallivm_state *gallivm, const char *name,
                   lp_context_ref *context, struct lp_cached_code *cache)
{
   assert(!gallivm->context);
   assert(!gallivm->module);

   if (!lp_build_init())
      return false;

   gallivm->context = context->ref;
   gallivm->cache = cache;
   if (!gallivm->context)
      goto fail;

   gallivm->module_name = NULL;
   if (name) {
      size_t size = strlen(name) + 1;
      gallivm->module_name = MALLOC(size);
      if (gallivm->module_name) {
         memcpy(gallivm->module_name, name, size);
      }
   }

   gallivm->module = LLVMModuleCreateWithNameInContext(name,
                                                       gallivm->context);
   if (!gallivm->module)
      goto fail;

#if DETECT_ARCH_X86
   lp_set_module_stack_alignment_override(gallivm->module, 4);
#endif

   gallivm->builder = LLVMCreateBuilderInContext(gallivm->context);
   if (!gallivm->builder)
      goto fail;

   gallivm->memorymgr = lp_get_default_memory_manager();
   if (!gallivm->memorymgr)
      goto fail;

   /* FIXME: MC-JIT only allows compiling one module at a time, and it must be
    * complete when MC-JIT is created. So defer the MC-JIT engine creation for
    * now.
    */

   /*
    * MC-JIT engine compiles the module immediately on creation, so we can't
    * obtain the target data from it.  Instead we create a target data layout
    * from a string.
    *
    * The produced layout strings are not precisely the same, but should make
    * no difference for the kind of optimization passes we run.
    *
    * For reference this is the layout string on x64:
    *
    *   e-p:64:64:64-S128-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f16:16:16-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-f128:128:128-n8:16:32:64
    *
    * See also:
    * - http://llvm.org/docs/LangRef.html#datalayout
    */

   {
      const unsigned pointer_size = 8 * sizeof(void *);
      char layout[512];
      snprintf(layout, sizeof layout, "%c-p:%u:%u:%u-i64:64:64-a0:0:%u-s0:%u:%u",
#if UTIL_ARCH_LITTLE_ENDIAN
                    'e', // little endian
#else
                    'E', // big endian
#endif
                    pointer_size, pointer_size, pointer_size, // pointer size, abi alignment, preferred alignment
                    pointer_size, // aggregate preferred alignment
                    pointer_size, pointer_size); // stack objects abi alignment, preferred alignment

      gallivm->target = LLVMCreateTargetData(layout);
      if (!gallivm->target) {
         return false;
      }
   }

   if (!create_pass_manager(gallivm))
      goto fail;

   if (gallivm_debug & GALLIVM_DEBUG_SYMBOLS)
      gallivm->di_builder = LLVMCreateDIBuilder(gallivm->module);

   lp_build_coro_declare_malloc_hooks(gallivm);
   return true;

fail:
   gallivm_free_ir(gallivm);
   gallivm_free_code(gallivm);
   return false;
}

bool
lp_build_init(void)
{
   lp_build_init_native_width();
   if (gallivm_initialized)
      return true;


   /* LLVMLinkIn* are no-ops at runtime.  They just ensure the respective
    * component is linked at buildtime, which is sufficient for its static
    * constructors to be called at load time.
    */
   LLVMLinkInMCJIT();

   lp_init_env_options();

   lp_set_target_options();

   lp_bld_ppc_disable_denorms();

   gallivm_initialized = true;

   return true;
}



/**
 * Create a new gallivm_state object.
 */
struct gallivm_state *
gallivm_create(const char *name, lp_context_ref *context,
               struct lp_cached_code *cache)
{
   struct gallivm_state *gallivm;

   gallivm = CALLOC_STRUCT(gallivm_state);
   if (gallivm) {
      if (!init_gallivm_state(gallivm, name, context, cache)) {
         FREE(gallivm);
         gallivm = NULL;
      }
   }

   assert(gallivm != NULL);
   return gallivm;
}


/**
 * Destroy a gallivm_state object.
 */
void
gallivm_destroy(struct gallivm_state *gallivm)
{
   gallivm_free_ir(gallivm);
   if (gallivm->engine)
      LLVMDisposeExecutionEngine(gallivm->engine);
   gallivm_free_code(gallivm);
   FREE(gallivm);
}

void
gallivm_add_global_mapping(struct gallivm_state *gallivm, LLVMValueRef sym, void* addr)
{
   LLVMAddGlobalMapping(gallivm->engine, sym, addr);
}

/**
 * Compile a module.
 * This does IR optimization on all functions in the module.
 */
void
gallivm_compile_module(struct gallivm_state *gallivm)
{
   assert(!gallivm->compiled);

   if (gallivm->builder) {
      LLVMDisposeBuilder(gallivm->builder);
      gallivm->builder = NULL;
   }

   if (gallivm->di_builder) {
      LLVMDIBuilderFinalize(gallivm->di_builder);
      LLVMDisposeDIBuilder(gallivm->di_builder);
      gallivm->di_builder = NULL;
   }

   LLVMSetDataLayout(gallivm->module, "");
   assert(!gallivm->engine);
   if (!init_gallivm_engine(gallivm)) {
      assert(0);
   }
   assert(gallivm->engine);

   if (gallivm->cache && gallivm->cache->data_size) {
      goto skip_cached;
   }

   /* Dump bitcode to a file */
   if (gallivm_debug & GALLIVM_DEBUG_DUMP_BC) {
      char filename[256];
      assert(gallivm->module_name);
      snprintf(filename, sizeof(filename), "ir_%s.bc", gallivm->module_name);
      LLVMWriteBitcodeToFile(gallivm->module, filename);
      debug_printf("%s written\n", filename);
      debug_printf("Invoke as \"opt %s %s | llc -O%d %s%s\"\n",
                   gallivm_perf & GALLIVM_PERF_NO_OPT ? "-mem2reg" :
                   "-sroa -early-cse -simplifycfg -reassociate "
                   "-mem2reg -constprop -instcombine -gvn",
                   filename, gallivm_perf & GALLIVM_PERF_NO_OPT ? 0 : 2,
                   "[-mcpu=<-mcpu option>] ",
                   "[-mattr=<-mattr option(s)>]");
   }

   lp_passmgr_run(gallivm->passmgr,
                  gallivm->module,
                  LLVMGetExecutionEngineTargetMachine(gallivm->engine),
                  gallivm->module_name);

   /* Setting the module's DataLayout to an empty string will cause the
    * ExecutionEngine to copy to the DataLayout string from its target machine
    * to the module.  As of LLVM 3.8 the module and the execution engine are
    * required to have the same DataLayout.
    *
    * We must make sure we do this after running the optimization passes,
    * because those passes need a correct datalayout string.  For example, if
    * those optimization passes see an empty datalayout, they will assume this
    * is a little endian target and will do optimizations that break big endian
    * machines.
    *
    * TODO: This is just a temporary work-around.  The correct solution is for
    * gallivm_init_state() to create a TargetMachine and pull the DataLayout
    * from there.  Currently, the TargetMachine used by llvmpipe is being
    * implicitly created by the EngineBuilder in
    * lp_build_create_jit_compiler_for_module()
    */
 skip_cached:

   ++gallivm->compiled;

   lp_init_printf_hook(gallivm);
   gallivm_add_global_mapping(gallivm, gallivm->debug_printf_hook, debug_printf);

   lp_init_clock_hook(gallivm);
   gallivm_add_global_mapping(gallivm, gallivm->get_time_hook, os_time_get_nano);

   lp_build_coro_add_malloc_hooks(gallivm);

   if (gallivm_debug & GALLIVM_DEBUG_ASM) {
      LLVMValueRef llvm_func = LLVMGetFirstFunction(gallivm->module);

      while (llvm_func) {
         /*
          * Need to filter out functions which don't have an implementation,
          * such as the intrinsics. May not be sufficient in case of IPO?
          * LLVMGetPointerToGlobal() will abort otherwise.
          */
         if (!LLVMIsDeclaration(llvm_func)) {
            void *func_code = LLVMGetPointerToGlobal(gallivm->engine, llvm_func);
            lp_disassemble(llvm_func, func_code);
         }
         llvm_func = LLVMGetNextFunction(llvm_func);
      }
   }

#if defined(PROFILE)
   {
      LLVMValueRef llvm_func = LLVMGetFirstFunction(gallivm->module);

      while (llvm_func) {
         if (!LLVMIsDeclaration(llvm_func)) {
            void *func_code = LLVMGetPointerToGlobal(gallivm->engine, llvm_func);
            lp_profile(llvm_func, func_code);
         }
         llvm_func = LLVMGetNextFunction(llvm_func);
      }
   }
#endif
}



func_pointer
gallivm_jit_function(struct gallivm_state *gallivm,
                     LLVMValueRef func, const char *func_name)
{
   void *code;
   func_pointer jit_func;
   int64_t time_begin = 0;

   assert(gallivm->compiled);
   assert(gallivm->engine);

   if (gallivm_debug & GALLIVM_DEBUG_PERF)
      time_begin = os_time_get();

   code = LLVMGetPointerToGlobal(gallivm->engine, func);
   assert(code);
   jit_func = pointer_to_func(code);

   if (gallivm_debug & GALLIVM_DEBUG_PERF) {
      int64_t time_end = os_time_get();
      int time_msec = (int)(time_end - time_begin) / 1000;
      debug_printf("   jitting func %s took %d msec\n",
                   LLVMGetValueName(func), time_msec);
   }

   return jit_func;
}

void
gallivm_stub_func(struct gallivm_state *gallivm, LLVMValueRef func)
{
   /*
    * MCJIT can accept an empty function, nothing is needed here.
    * The only code is to silence unused var warning.
    */
   (void) gallivm;
   (void) func;
}
