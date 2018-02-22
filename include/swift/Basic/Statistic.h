//===--- Statistic.h - Helpers for llvm::Statistic --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_STATISTIC_H
#define SWIFT_BASIC_STATISTIC_H

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Timer.h"

#define SWIFT_FUNC_STAT                                                 \
  do {                                                                  \
    static llvm::Statistic FStat =                                      \
      {DEBUG_TYPE, __func__, __func__, {0}, {false}};                   \
    ++FStat;                                                            \
  } while (0)

// Helper class designed to consolidate reporting of LLVM statistics and timers
// across swift compilations that typically invoke many drivers, each running
// many frontends. Additionally collects some cheap "always-on" statistics,
// beyond those that are (compile-time) parameterized by -DLLVM_ENABLE_STATS
// (LLVM's stats are global and involve some amount of locking and mfences).
//
// Assumes it's given a process name and target name (the latter used as
// decoration for its self-timer), and a directory to collect stats into, then:
//
//  - On construction:
//    - Calls llvm::EnableStatistics(/*PrintOnExit=*/false)
//    - Calls swift::enableCompilationTimers()
//    - Starts an llvm::NamedRegionTimer for this process
//
//  - On destruction:
//    - Add any standard always-enabled stats about the process as a whole
//    - Opens $dir/stats-$timestamp-$name-$random.json for writing
//    - Calls llvm::PrintStatisticsJSON(ostream) and/or its own writer
//
// Generally we make one of these per-process: either early in the life of the
// driver, or early in the life of the frontend.

namespace clang {
  class Decl;
  class SourceManager;
}

namespace swift {

class Decl;
class ProtocolConformance;
class Expr;
class SILFunction;
class FrontendStatsTracer;
class SourceManager;

class UnifiedStatsReporter {

public:
  struct AlwaysOnDriverCounters
  {
#define DRIVER_STATISTIC(ID) int64_t ID;
#include "Statistics.def"
#undef DRIVER_STATISTIC
  };

  struct AlwaysOnFrontendCounters
  {
#define FRONTEND_STATISTIC(NAME, ID) int64_t ID;
#include "Statistics.def"
#undef FRONTEND_STATISTIC
  };

  // To trace an entity, you have to provide a TraceFormatter for it. This is a
  // separate type since we do not have retroactive conformances in C++, and it
  // is a type that takes void* arguments since we do not have existentials
  // separate from objects in C++. Pity us.
  struct TraceFormatter {
    virtual void traceName(const void *Entity, raw_ostream &OS) const = 0;
    virtual void traceLoc(const void *Entity,
                          SourceManager *SourceMgr,
                          clang::SourceManager *ClangSourceMgr,
                          raw_ostream &OS) const = 0;
    virtual ~TraceFormatter();
  };

  struct FrontendStatsEvent
  {
    uint64_t TimeUSec;
    uint64_t LiveUSec;
    bool IsEntry;
    StringRef EventName;
    StringRef CounterName;
    int64_t CounterDelta;
    int64_t CounterValue;
    const void *Entity;
    const TraceFormatter *Formatter;
  };

  // We only write fine-grained trace entries when the user passed
  // -trace-stats-events, but we recycle the same FrontendStatsTracers to give
  // us some free recursion-save phase timings whenever -trace-stats-dir is
  // active at all. Reduces redundant machinery.
  class RecursionSafeTimers;

  // We also keep a few banks of optional hierarchical profilers for times and
  // statistics, activated with -profile-stats-events and
  // -profile-stats-entities, which are part way between the detail level of the
  // aggregate statistic JSON files and the fine-grained CSV traces. Naturally
  // these are written in yet a different file format: the input format for
  // flamegraphs.
  struct StatsProfilers;

private:
  bool currentProcessExitStatusSet;
  int currentProcessExitStatus;
  SmallString<128> StatsFilename;
  SmallString<128> TraceFilename;
  SmallString<128> ProfileDirname;
  llvm::TimeRecord StartedTime;

  // This is unique_ptr because NamedRegionTimer is non-copy-constructable.
  std::unique_ptr<llvm::NamedRegionTimer> Timer;

  SourceManager *SourceMgr;
  clang::SourceManager *ClangSourceMgr;
  Optional<AlwaysOnDriverCounters> DriverCounters;
  Optional<AlwaysOnFrontendCounters> FrontendCounters;
  Optional<AlwaysOnFrontendCounters> LastTracedFrontendCounters;
  Optional<std::vector<FrontendStatsEvent>> FrontendStatsEvents;

  // These are unique_ptr so we can use incomplete types here.
  std::unique_ptr<RecursionSafeTimers> RecursiveTimers;
  std::unique_ptr<StatsProfilers> EventProfilers;
  std::unique_ptr<StatsProfilers> EntityProfilers;

  void publishAlwaysOnStatsToLLVM();
  void printAlwaysOnStatsAndTimers(raw_ostream &OS);

  UnifiedStatsReporter(StringRef ProgramName,
                       StringRef AuxName,
                       StringRef Directory,
                       SourceManager *SM,
                       clang::SourceManager *CSM,
                       bool TraceEvents,
                       bool ProfileEvents,
                       bool ProfileEntities);
public:
  UnifiedStatsReporter(StringRef ProgramName,
                       StringRef ModuleName,
                       StringRef InputName,
                       StringRef TripleName,
                       StringRef OutputType,
                       StringRef OptType,
                       StringRef Directory,
                       SourceManager *SM=nullptr,
                       clang::SourceManager *CSM=nullptr,
                       bool TraceEvents=false,
                       bool ProfileEvents=false,
                       bool ProfileEntities=false);
  ~UnifiedStatsReporter();

  AlwaysOnDriverCounters &getDriverCounters();
  AlwaysOnFrontendCounters &getFrontendCounters();
  void noteCurrentProcessExitStatus(int);
  void saveAnyFrontendStatsEvents(FrontendStatsTracer const &T, bool IsEntry);
};

// This is a non-nested type just to make it less work to write at call sites.
class FrontendStatsTracer
{
  FrontendStatsTracer(UnifiedStatsReporter *Reporter,
                      StringRef EventName,
                      const void *Entity,
                      const UnifiedStatsReporter::TraceFormatter *Formatter);

  // In the general case we do not know how to format an entity for tracing.
  template<typename T> static
  const UnifiedStatsReporter::TraceFormatter *getTraceFormatter() {
    return nullptr;
  }

public:
  UnifiedStatsReporter *Reporter;
  llvm::TimeRecord SavedTime;
  StringRef EventName;
  const void *Entity;
  const UnifiedStatsReporter::TraceFormatter *Formatter;
  FrontendStatsTracer();
  FrontendStatsTracer(FrontendStatsTracer&& other);
  FrontendStatsTracer& operator=(FrontendStatsTracer&&);
  ~FrontendStatsTracer();
  FrontendStatsTracer(const FrontendStatsTracer&) = delete;
  FrontendStatsTracer& operator=(const FrontendStatsTracer&) = delete;

  /// These are the convenience constructors you want to be calling throughout
  /// the compiler: they select an appropriate trace formatter for the provided
  /// entity type, and produce a tracer that's either active or inert depending
  /// on whether the provided \p Reporter is null (nullptr means "tracing is
  /// disabled").
  FrontendStatsTracer(UnifiedStatsReporter *Reporter,  StringRef EventName);
  FrontendStatsTracer(UnifiedStatsReporter *Reporter,  StringRef EventName,
                      const Decl *D);
  FrontendStatsTracer(UnifiedStatsReporter *Reporter,  StringRef EventName,
                      const ProtocolConformance *P);
  FrontendStatsTracer(UnifiedStatsReporter *Reporter,  StringRef EventName,
                      const clang::Decl *D);
  FrontendStatsTracer(UnifiedStatsReporter *Reporter,  StringRef EventName,
                      const Expr *E);
  FrontendStatsTracer(UnifiedStatsReporter *Reporter,  StringRef EventName,
                      const SILFunction *F);
};

// In particular cases, we do know how to format traced entities: we declare
// explicit specializations of getTraceFormatter() here, matching the overloaded
// constructors of FrontendStatsTracer above, where the _definitions_ live in
// the upper-level files (in libswiftAST or libswiftSIL), and provide tracing
// for those entity types. If you want to trace those types, it's assumed you're
// linking with the object files that define the tracer.

template<> const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const Decl *>();

template<> const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const ProtocolConformance *>();

template<> const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const clang::Decl *>();

template<> const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const Expr *>();

template<> const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const SILFunction *>();

}
#endif // SWIFT_BASIC_STATISTIC_H
