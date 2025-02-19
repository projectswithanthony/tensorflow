/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/hlo/translate/stablehlo.h"

#include <memory>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "llvm/Support/LogicalResult.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"
#include "xla/debug_options_flags.h"
#include "xla/hlo/translate/hlo_to_mhlo/hlo_module_importer.h"
#include "xla/hlo/translate/mhlo_to_hlo/mlir_hlo_to_hlo.h"
#include "xla/hlo/translate/mhlo_to_hlo/module_attributes_exporter.h"
#include "xla/mlir/utils/error_util.h"
#include "xla/mlir_hlo/mhlo/transforms/passes.h"
#include "xla/service/hlo.pb.h"
#include "xla/service/hlo_module_config.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "tsl/platform/errors.h"

namespace xla {

absl::StatusOr<mlir::OwningOpRef<mlir::ModuleOp>> ConvertHloToStablehlo(
    mlir::MLIRContext& ctx, const xla::HloModule* hlo_module) {
  mlir::OwningOpRef<mlir::ModuleOp> mlir_module =
      llvm_ir::CreateMlirModuleOp(mlir::UnknownLoc::get(&ctx));
  mlir::BaseScopedDiagnosticHandler diag_handler(&ctx);
  absl::Status status =
      HloModuleImporter(mlir_module.get(), /*import_all_computation*/ true,
                        /*flatten_computation_args_result*/ true)
          .Import(*hlo_module);
  if (!status.ok()) {
    return status;
  }
  mlir::PassManager pm(&ctx);
  pm.addPass(mlir::mhlo::createHloLegalizeToStablehloPass());
  if (failed(pm.run(*mlir_module))) {
    return diag_handler.ConsumeStatus();
  }
  return std::move(mlir_module);
}

absl::StatusOr<mlir::OwningOpRef<mlir::ModuleOp>> ConvertHloToStablehlo(
    mlir::MLIRContext& ctx, const xla::HloModuleProto* hlo_module_proto) {
  auto hlo_module = HloModule::CreateFromProto(*hlo_module_proto, {});
  if (!hlo_module.ok()) {
    return hlo_module.status();
  }
  return ConvertHloToStablehlo(ctx, hlo_module.value().get());
}

absl::StatusOr<std::unique_ptr<xla::HloModule>> ConvertStablehloToHlo(
    mlir::ModuleOp module) {
  xla::HloProto hlo_proto;
  TF_RETURN_IF_ERROR(ConvertStablehloToHloProto(module, &hlo_proto));

  // Create default config and modify config with values stored
  // in MLIR module attributes
  const xla::HloModuleProto& module_proto = hlo_proto.hlo_module();
  auto config = xla::HloModule::CreateModuleConfigFromProto(
      module_proto, xla::GetDebugOptionsFromFlags());
  if (!config.ok()) {
    return config.status();
  }
  mlir::mhlo::ExportHloModuleConfig(config.value(), module);

  return xla::HloModule::CreateFromProto(module_proto, config.value());
}

absl::Status ConvertStablehloToHloProto(mlir::ModuleOp module,
                                        xla::HloProto* hlo_proto) {
  if (!module) return absl::InvalidArgumentError("Module is null");

  mlir::MLIRContext* context = module->getContext();
  mlir::BaseScopedDiagnosticHandler diagnostic_handler(context);
  {
    mlir::PassManager pm(context);
    pm.addPass(mlir::mhlo::createStablehloLegalizeToHloPass());
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::mhlo::createChloLegalizeToHloPass());
    pm.addNestedPass<mlir::func::FuncOp>(mlir::createCanonicalizerPass());
    // In order to export to XLA, we must sink constants to control flow
    // regions, since XLA uses functional control flow.
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::mhlo::createSinkConstantsToControlFlowPass());
    if (failed(pm.run(module))) {
      VLOG(1) << "MHLO->HLO lowering passes failed.";
      module->dump();
      return diagnostic_handler.ConsumeStatus();
    }

    VLOG(5) << "MHLO module after lowering, before HLO import ";
    if (VLOG_IS_ON(5)) {
      module->dump();
    }
  }

  mlir::MlirToHloConversionOptions options;
  TF_RETURN_IF_ERROR(mlir::ConvertMlirHloToHlo(module, hlo_proto, options));
  return absl::OkStatus();
}

}  // namespace xla
