//===-- AnalysisManager.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisManager.h"

using namespace clang;
using namespace ento;

void AnalysisManager::anchor() { }

AnalysisManager::AnalysisManager(ASTContext &ASTCtx,
                                 const PathDiagnosticConsumers &PDC,
                                 StoreManagerCreator storemgr,
                                 ConstraintManagerCreator constraintmgr,
                                 CheckerManager *checkerMgr,
                                 AnalyzerOptions &Options,
                                 CodeInjector *injector)
    : AnaCtxMgr(
          ASTCtx, Options.UnoptimizedCFG,
          Options.ShouldIncludeImplicitDtorsInCFG,
          /*AddInitializers=*/true,
          Options.ShouldIncludeTemporaryDtorsInCFG,
          Options.ShouldIncludeLifetimeInCFG,
          // Adding LoopExit elements to the CFG is a requirement for loop
          // unrolling.
          Options.ShouldIncludeLoopExitInCFG ||
            Options.ShouldUnrollLoops,
          Options.ShouldIncludeScopesInCFG,
          Options.ShouldSynthesizeBodies,
          Options.ShouldConditionalizeStaticInitializers,
          /*addCXXNewAllocator=*/true,
          Options.ShouldIncludeRichConstructorsInCFG,
          Options.ShouldElideConstructors,
          /*addVirtualBaseBranches=*/true,
          injector),
      Ctx(ASTCtx), LangOpts(ASTCtx.getLangOpts()),
      PathConsumers(PDC), CreateStoreMgr(storemgr),
      CreateConstraintMgr(constraintmgr), CheckerMgr(checkerMgr),
      options(Options) {
  AnaCtxMgr.getCFGBuildOptions().setAllAlwaysAdd();
}

AnalysisManager::~AnalysisManager() {
  FlushDiagnostics();
  for (PathDiagnosticConsumers::iterator I = PathConsumers.begin(),
       E = PathConsumers.end(); I != E; ++I) {
    delete *I;
  }
}

void AnalysisManager::FlushDiagnostics() {
  PathDiagnosticConsumer::FilesMade filesMade;
  for (PathDiagnosticConsumers::iterator I = PathConsumers.begin(),
       E = PathConsumers.end();
       I != E; ++I) {
    (*I)->FlushDiagnostics(&filesMade);
  }
}
