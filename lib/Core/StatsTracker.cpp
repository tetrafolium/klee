//===-- StatsTracker.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "StatsTracker.h"

#include "ExecutionState.h"

#include "klee/Config/Version.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/Solver/SolverStats.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "klee/System/MemoryUsage.h"

#include "CallPathManager.h"
#include "CoreStats.h"
#include "Executor.h"
#include "MemoryManager.h"
#include "UserSearcher.h"

#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

#include <fstream>
#include <unistd.h>

using namespace klee;
using namespace llvm;

///

namespace {
cl::OptionCategory
        StatsCat("Statistics options",
                 "These options control the statistics generated by KLEE.");

cl::opt<bool> TrackInstructionTime(
	"track-instruction-time", cl::init(false),
	cl::desc(
		"Enable tracking of time for individual instructions (default=false)"),
	cl::cat(StatsCat));

cl::opt<bool>
OutputStats("output-stats", cl::init(true),
            cl::desc("Write running stats trace file (default=true)"),
            cl::cat(StatsCat));

cl::opt<bool> OutputIStats("output-istats", cl::init(true),
                           cl::desc("Write instruction level statistics in "
                                    "callgrind format (default=true)"),
                           cl::cat(StatsCat));

cl::opt<std::string> StatsWriteInterval(
	"stats-write-interval", cl::init("1s"),
	cl::desc("Approximate time between stats writes (default=1s)"),
	cl::cat(StatsCat));

cl::opt<unsigned> StatsWriteAfterInstructions(
	"stats-write-after-instructions", cl::init(0),
	cl::desc(
		"Write statistics after each n instructions, 0 to disable (default=0)"),
	cl::cat(StatsCat));

cl::opt<unsigned> CommitEvery(
	"stats-commit-after", cl::init(0),
	cl::desc("Commit the statistics every N writes. By default commit every "
	         "write with -stats-write-interval or every 1000 writes with "
	         "-stats-write-after-instructions. (default=0)"),
	cl::cat(StatsCat));

cl::opt<std::string> IStatsWriteInterval(
	"istats-write-interval", cl::init("10s"),
	cl::desc(
		"Approximate number of seconds between istats writes (default=10s)"),
	cl::cat(StatsCat));

cl::opt<unsigned> IStatsWriteAfterInstructions(
	"istats-write-after-instructions", cl::init(0),
	cl::desc(
		"Write istats after each n instructions, 0 to disable (default=0)"),
	cl::cat(StatsCat));

// XXX I really would like to have dynamic rate control for something like this.
cl::opt<std::string> UncoveredUpdateInterval(
	"uncovered-update-interval", cl::init("30s"),
	cl::desc("Update interval for uncovered instructions (default=30s)"),
	cl::cat(StatsCat));

cl::opt<bool> UseCallPaths("use-call-paths", cl::init(true),
                           cl::desc("Enable calltree tracking for instruction "
                                    "level statistics (default=true)"),
                           cl::cat(StatsCat));

} // namespace

///

bool StatsTracker::useStatistics() {
	return OutputStats || OutputIStats;
}

bool StatsTracker::useIStats() {
	return OutputIStats;
}

/// Check for special cases where we statically know an instruction is
/// uncoverable. Currently the case is an unreachable instruction
/// following a noreturn call; the instruction is really only there to
/// satisfy LLVM's termination requirement.
static bool instructionIsCoverable(Instruction *i) {
	if (i->getOpcode() == Instruction::Unreachable) {
		BasicBlock *bb = i->getParent();
		BasicBlock::iterator it(i);
		if (it == bb->begin()) {
			return true;
		} else {
			Instruction *prev = &*(--it);
			if (isa<CallInst>(prev) || isa<InvokeInst>(prev)) {
				Function *target =
					getDirectCallTarget(CallSite(prev), /*moduleIsFullyLinked=*/ true);
				if (target && target->doesNotReturn())
					return false;
			}
		}
	}

	return true;
}

std::string sqlite3ErrToStringAndFree(const std::string &prefix,
                                      char *sqlite3ErrMsg) {
	std::ostringstream sstream;
	sstream << prefix << sqlite3ErrMsg;
	sqlite3_free(sqlite3ErrMsg);
	return sstream.str();
}

StatsTracker::StatsTracker(Executor &_executor, std::string _objectFilename,
                           bool _updateMinDistToUncovered)
	: executor(_executor), objectFilename(_objectFilename),
	startWallTime(time::getWallTime()), numBranches(0), fullBranches(0),
	partialBranches(0), updateMinDistToUncovered(_updateMinDistToUncovered) {

	const time::Span statsWriteInterval(StatsWriteInterval);
	if (StatsWriteAfterInstructions > 0 && statsWriteInterval)
		klee_error("Both options --stats-write-interval and "
		           "--stats-write-after-instructions cannot be enabled at the same "
		           "time.");

	const time::Span iStatsWriteInterval(IStatsWriteInterval);
	if (IStatsWriteAfterInstructions > 0 && iStatsWriteInterval)
		klee_error(
			"Both options --istats-write-interval and "
			"--istats-write-after-instructions cannot be enabled at the same "
			"time.");

	KModule *km = executor.kmodule.get();
	if (CommitEvery > 0) {
		statsCommitEvery = CommitEvery;
	} else {
		statsCommitEvery = statsWriteInterval ? 1 : 1000;
	}

	if (!sys::path::is_absolute(objectFilename)) {
		SmallString<128> current(objectFilename);
		if (sys::fs::make_absolute(current)) {
			Twine current_twine(current.str()); // requires a twine for this
			if (!sys::fs::exists(current_twine)) {
				objectFilename = current.c_str();
			}
		}
	}

	if (useStatistics() || userSearcherRequiresMD2U())
		theStatisticManager->useIndexedStats(km->infos->getMaxID());

	for (auto &kfp : km->functions) {
		KFunction *kf = kfp.get();
		kf->trackCoverage = 1;

		for (unsigned i = 0; i < kf->numInstructions; ++i) {
			KInstruction *ki = kf->instructions[i];

			if (OutputIStats) {
				unsigned id = ki->info->id;
				theStatisticManager->setIndex(id);
				if (kf->trackCoverage && instructionIsCoverable(ki->inst))
					++stats::uncoveredInstructions;
			}

			if (kf->trackCoverage) {
				if (BranchInst *bi = dyn_cast<BranchInst>(ki->inst))
					if (!bi->isUnconditional())
						numBranches++;
			}
		}
	}

	if (OutputStats) {
		sqlite3_config(SQLITE_CONFIG_SINGLETHREAD);
		sqlite3_enable_shared_cache(0);

		// open database
		auto db_filename =
			executor.interpreterHandler->getOutputFilename("run.stats");
		if (sqlite3_open(db_filename.c_str(), &statsFile) != SQLITE_OK) {
			std::ostringstream errorstream;
			errorstream << "Can't open database: " << sqlite3_errmsg(statsFile);
			sqlite3_close(statsFile);
			klee_error("%s", errorstream.str().c_str());
		}

		// prepare statements
		if (sqlite3_prepare_v2(statsFile, "BEGIN TRANSACTION", -1,
		                       &transactionBeginStmt, nullptr) != SQLITE_OK) {
			klee_error("Cannot create prepared statement: %s",
			           sqlite3_errmsg(statsFile));
		}

		if (sqlite3_prepare_v2(statsFile, "END TRANSACTION", -1,
		                       &transactionEndStmt, nullptr) != SQLITE_OK) {
			klee_error("Cannot create prepared statement: %s",
			           sqlite3_errmsg(statsFile));
		}

		// set options
		char *zErrMsg;
		if (sqlite3_exec(statsFile, "PRAGMA synchronous = OFF", nullptr, nullptr,
		                 &zErrMsg) != SQLITE_OK) {
			klee_error("%s", sqlite3ErrToStringAndFree(
					   "Can't set options for database: ", zErrMsg)
			           .c_str());
		}

		// note: we use WAL here a) for speed and b) to prevent creation of new file
		// descriptors (as with TRUNCATE)
		if (sqlite3_exec(statsFile, "PRAGMA journal_mode = WAL", nullptr, nullptr,
		                 &zErrMsg) != SQLITE_OK) {
			klee_error("%s", sqlite3ErrToStringAndFree(
					   "Can't set options for database: ", zErrMsg)
			           .c_str());
		}

		// create table
		writeStatsHeader();

		// begin transaction
		auto rc = sqlite3_step(transactionBeginStmt);
		if (rc != SQLITE_DONE) {
			klee_warning("Can't begin transaction: %s", sqlite3_errmsg(statsFile));
		}
		sqlite3_reset(transactionBeginStmt);

		writeStatsLine();

		if (statsWriteInterval)
			executor.timers.add(std::make_unique<Timer>(statsWriteInterval,
			                                            [&] {
				writeStatsLine();
			}));
	}

	// Add timer to calculate uncovered instructions if needed by the solver
	if (updateMinDistToUncovered) {
		computeReachableUncovered();
		executor.timers.add(
			std::make_unique<Timer>(time::Span{UncoveredUpdateInterval},
			                        [&] {
			computeReachableUncovered();
		}));
	}

	if (OutputIStats) {
		istatsFile = executor.interpreterHandler->openOutputFile("run.istats");
		if (istatsFile) {
			if (iStatsWriteInterval)
				executor.timers.add(std::make_unique<Timer>(iStatsWriteInterval,
				                                            [&] {
					writeIStats();
				}));
		} else {
			klee_error("Unable to open instruction level stats file (run.istats).");
		}
	}
}

StatsTracker::~StatsTracker() {
	if (statsFile) {
		auto rc = sqlite3_step(transactionEndStmt);
		if (rc != SQLITE_DONE) {
			klee_warning("Can't commit transaction: %s", sqlite3_errmsg(statsFile));
		}
		sqlite3_reset(transactionEndStmt);
		sqlite3_finalize(transactionBeginStmt);
		sqlite3_finalize(transactionEndStmt);
		sqlite3_finalize(insertStmt);
		sqlite3_close(statsFile);
	}
}

void StatsTracker::done() {
	if (statsFile)
		writeStatsLine();

	if (OutputIStats) {
		if (updateMinDistToUncovered)
			computeReachableUncovered();
		if (istatsFile)
			writeIStats();
	}
}

void StatsTracker::stepInstruction(ExecutionState &es) {
	if (OutputIStats) {
		if (TrackInstructionTime) {
			static time::Point lastNowTime(time::getWallTime());
			static time::Span lastUserTime;

			if (!lastUserTime) {
				lastUserTime = time::getUserTime();
			} else {
				const auto now = time::getWallTime();
				const auto user = time::getUserTime();
				const auto delta = user - lastUserTime;
				const auto deltaNow = now - lastNowTime;
				stats::instructionTime += delta.toMicroseconds();
				stats::instructionRealTime += deltaNow.toMicroseconds();
				lastUserTime = user;
				lastNowTime = now;
			}
		}

		Instruction *inst = es.pc->inst;
		const InstructionInfo &ii = *es.pc->info;
		StackFrame &sf = es.stack.back();
		theStatisticManager->setIndex(ii.id);
		if (UseCallPaths)
			theStatisticManager->setContext(&sf.callPathNode->statistics);

		if (es.instsSinceCovNew)
			++es.instsSinceCovNew;

		if (sf.kf->trackCoverage && instructionIsCoverable(inst)) {
			if (!theStatisticManager->getIndexedValue(stats::coveredInstructions,
			                                          ii.id)) {
				// Checking for actual stoppoints avoids inconsistencies due
				// to line number propogation.
				//
				// FIXME: This trick no longer works, we should fix this in the line
				// number propogation.
				es.coveredLines[&ii.file].insert(ii.line);
				es.coveredNew = true;
				es.instsSinceCovNew = 1;
				++stats::coveredInstructions;
				stats::uncoveredInstructions += (uint64_t)-1;
			}
		}
	}

	if (statsFile && StatsWriteAfterInstructions &&
	    stats::instructions % StatsWriteAfterInstructions.getValue() == 0)
		writeStatsLine();

	if (istatsFile && IStatsWriteAfterInstructions &&
	    stats::instructions % IStatsWriteAfterInstructions.getValue() == 0)
		writeIStats();
}

///

/* Should be called _after_ the es->pushFrame() */
void StatsTracker::framePushed(ExecutionState &es, StackFrame *parentFrame) {
	if (OutputIStats) {
		StackFrame &sf = es.stack.back();

		if (UseCallPaths) {
			CallPathNode *parent = parentFrame ? parentFrame->callPathNode : 0;
			CallPathNode *cp = callPathManager.getCallPath(
				parent, sf.caller ? sf.caller->inst : 0, sf.kf->function);
			sf.callPathNode = cp;
			cp->count++;
		}
	}

	if (updateMinDistToUncovered) {
		StackFrame &sf = es.stack.back();

		uint64_t minDistAtRA = 0;
		if (parentFrame)
			minDistAtRA = parentFrame->minDistToUncoveredOnReturn;

		sf.minDistToUncoveredOnReturn =
			sf.caller ? computeMinDistToUncovered(sf.caller, minDistAtRA) : 0;
	}
}

/* Should be called _after_ the es->popFrame() */
void StatsTracker::framePopped(ExecutionState &es) {
	// XXX remove me?
}

void StatsTracker::markBranchVisited(ExecutionState *visitedTrue,
                                     ExecutionState *visitedFalse) {
	if (OutputIStats) {
		unsigned id = theStatisticManager->getIndex();
		uint64_t hasTrue =
			theStatisticManager->getIndexedValue(stats::trueBranches, id);
		uint64_t hasFalse =
			theStatisticManager->getIndexedValue(stats::falseBranches, id);
		if (visitedTrue && !hasTrue) {
			visitedTrue->coveredNew = true;
			visitedTrue->instsSinceCovNew = 1;
			++stats::trueBranches;
			if (hasFalse) {
				++fullBranches;
				--partialBranches;
			} else
				++partialBranches;
			hasTrue = 1;
		}
		if (visitedFalse && !hasFalse) {
			visitedFalse->coveredNew = true;
			visitedFalse->instsSinceCovNew = 1;
			++stats::falseBranches;
			if (hasTrue) {
				++fullBranches;
				--partialBranches;
			} else
				++partialBranches;
		}
	}
}

void StatsTracker::writeStatsHeader() {
	std::ostringstream create, insert;
	create << "CREATE TABLE stats ("
	       << "Instructions INTEGER,"
	       << "FullBranches INTEGER,"
	       << "PartialBranches INTEGER,"
	       << "NumBranches INTEGER,"
	       << "UserTime REAL,"
	       << "NumStates INTEGER,"
	       << "MallocUsage INTEGER,"
	       << "NumQueries INTEGER,"
	       << "NumQueryConstructs INTEGER,"
	       << "WallTime REAL,"
	       << "CoveredInstructions INTEGER,"
	       << "UncoveredInstructions INTEGER,"
	       << "QueryTime INTEGER,"
	       << "SolverTime INTEGER,"
	       << "CexCacheTime INTEGER,"
	       << "ForkTime INTEGER,"
	       << "ResolveTime INTEGER,"
	       << "QueryCexCacheMisses INTEGER,"
	       << "QueryCexCacheHits INTEGER,"
	       << "ArrayHashTime INTEGER" << ')';
	char *zErrMsg = nullptr;
	if (sqlite3_exec(statsFile, create.str().c_str(), nullptr, nullptr,
	                 &zErrMsg)) {
		klee_error(
			"%s",
			sqlite3ErrToStringAndFree("ERROR creating table: ", zErrMsg).c_str());
	}
	/* Sometimes KLEE runs out of file descriptors and hence we try to a) keep
	 * important fds open and b) prevent the creation of temporary files. SQLite3
	 * uses temporary files for statement journals, which help rollbacks when
	 * constraints are violated. We have no constraints in our table so there
	 * shouldn't be a constraint violation. `OR FAIL` will not write to temp files
	 * and therefore not rollback but simply fail. As said before this should not
	 * happen, but if it does this statement will fail with SQLITE_CONSTRAINT
	 * error. If this happens you should either remove the constraints or consider
	 * using `IGNORE` mode.
	 */
	insert << "INSERT OR FAIL INTO stats ("
	       << "Instructions,"
	       << "FullBranches,"
	       << "PartialBranches,"
	       << "NumBranches,"
	       << "UserTime,"
	       << "NumStates,"
	       << "MallocUsage,"
	       << "NumQueries,"
	       << "NumQueryConstructs,"
	       << "WallTime,"
	       << "CoveredInstructions,"
	       << "UncoveredInstructions,"
	       << "QueryTime,"
	       << "SolverTime,"
	       << "CexCacheTime,"
	       << "ForkTime,"
	       << "ResolveTime,"
	       << "QueryCexCacheMisses,"
	       << "QueryCexCacheHits,"
	       << "ArrayHashTime"
	       << ") VALUES ("
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "?,"
	       << "? " << ')';

	if (sqlite3_prepare_v2(statsFile, insert.str().c_str(), -1, &insertStmt,
	                       nullptr) != SQLITE_OK) {
		klee_error("Cannot create prepared statement: %s",
		           sqlite3_errmsg(statsFile));
	}
}

time::Span StatsTracker::elapsed() {
	return time::getWallTime() - startWallTime;
}

void StatsTracker::writeStatsLine() {
	sqlite3_bind_int64(insertStmt, 1, stats::instructions);
	sqlite3_bind_int64(insertStmt, 2, fullBranches);
	sqlite3_bind_int64(insertStmt, 3, partialBranches);
	sqlite3_bind_int64(insertStmt, 4, numBranches);
	sqlite3_bind_int64(insertStmt, 5, time::getUserTime().toMicroseconds());
	sqlite3_bind_int64(insertStmt, 6, executor.states.size());
	sqlite3_bind_int64(insertStmt, 7,
	                   util::GetTotalMallocUsage() +
	                   executor.memory->getUsedDeterministicSize());
	sqlite3_bind_int64(insertStmt, 8, stats::queries);
	sqlite3_bind_int64(insertStmt, 9, stats::queryConstructs);
	sqlite3_bind_int64(insertStmt, 10, elapsed().toMicroseconds());
	sqlite3_bind_int64(insertStmt, 11, stats::coveredInstructions);
	sqlite3_bind_int64(insertStmt, 12, stats::uncoveredInstructions);
	sqlite3_bind_int64(insertStmt, 13, stats::queryTime);
	sqlite3_bind_int64(insertStmt, 14, stats::solverTime);
	sqlite3_bind_int64(insertStmt, 15, stats::cexCacheTime);
	sqlite3_bind_int64(insertStmt, 16, stats::forkTime);
	sqlite3_bind_int64(insertStmt, 17, stats::resolveTime);
	sqlite3_bind_int64(insertStmt, 18, stats::queryCexCacheMisses);
	sqlite3_bind_int64(insertStmt, 19, stats::queryCexCacheHits);
#ifdef KLEE_ARRAY_DEBUG
	sqlite3_bind_int64(insertStmt, 20, stats::arrayHashTime);
#else
	sqlite3_bind_int64(insertStmt, 20, -1LL);
#endif
	int errCode = sqlite3_step(insertStmt);
	if (errCode != SQLITE_DONE)
		klee_error("Error writing stats data: %s", sqlite3_errmsg(statsFile));
	sqlite3_reset(insertStmt);

	statsWriteCount++;
	if (statsWriteCount == statsCommitEvery) {
		errCode = sqlite3_step(transactionEndStmt);
		if (errCode != SQLITE_DONE)
			klee_warning("Transaction commit error: %s", sqlite3_errmsg(statsFile));
		sqlite3_reset(transactionEndStmt);
		errCode = sqlite3_step(transactionBeginStmt);
		if (errCode != SQLITE_DONE)
			klee_warning("Transaction begin error: %s", sqlite3_errmsg(statsFile));
		sqlite3_reset(transactionBeginStmt);

		statsWriteCount = 0;
	}
}

void StatsTracker::updateStateStatistics(uint64_t addend) {
	for (std::set<ExecutionState *>::iterator it = executor.states.begin(),
	     ie = executor.states.end();
	     it != ie; ++it) {
		ExecutionState &state = **it;
		const InstructionInfo &ii = *state.pc->info;
		theStatisticManager->incrementIndexedValue(stats::states, ii.id, addend);
		if (UseCallPaths)
			state.stack.back().callPathNode->statistics.incrementValue(stats::states,
			                                                           addend);
	}
}

void StatsTracker::writeIStats() {
	const auto m = executor.kmodule->module.get();
	llvm::raw_fd_ostream &of = *istatsFile;

	// We assume that we didn't move the file pointer
	unsigned istatsSize = of.tell();

	of.seek(0);

	of << "version: 1\n";
	of << "creator: klee\n";
	of << "pid: " << getpid() << "\n";
	of << "cmd: " << m->getModuleIdentifier() << "\n\n";
	of << "\n";

	StatisticManager &sm = *theStatisticManager;
	unsigned nStats = sm.getNumStatistics();
	llvm::SmallBitVector istatsMask(nStats);

	istatsMask.set(sm.getStatisticID("Queries"));
	istatsMask.set(sm.getStatisticID("QueriesValid"));
	istatsMask.set(sm.getStatisticID("QueriesInvalid"));
	istatsMask.set(sm.getStatisticID("QueryTime"));
	istatsMask.set(sm.getStatisticID("ResolveTime"));
	istatsMask.set(sm.getStatisticID("Instructions"));
	istatsMask.set(sm.getStatisticID("InstructionTimes"));
	istatsMask.set(sm.getStatisticID("InstructionRealTimes"));
	istatsMask.set(sm.getStatisticID("Forks"));
	istatsMask.set(sm.getStatisticID("CoveredInstructions"));
	istatsMask.set(sm.getStatisticID("UncoveredInstructions"));
	istatsMask.set(sm.getStatisticID("States"));
	istatsMask.set(sm.getStatisticID("MinDistToUncovered"));

	of << "positions: instr line\n";

	for (unsigned i = 0; i < nStats; i++) {
		if (istatsMask.test(i)) {
			Statistic &s = sm.getStatistic(i);
			of << "event: " << s.getShortName() << " : " << s.getName() << "\n";
		}
	}

	of << "events: ";
	for (unsigned i = 0; i < nStats; i++) {
		if (istatsMask.test(i))
			of << sm.getStatistic(i).getShortName() << " ";
	}
	of << "\n";

	// set state counts, decremented after we process so that we don't
	// have to zero all records each time.
	if (istatsMask.test(stats::states.getID()))
		updateStateStatistics(1);

	std::string sourceFile = "";

	CallSiteSummaryTable callSiteStats;
	if (UseCallPaths)
		callPathManager.getSummaryStatistics(callSiteStats);

	of << "ob=" << llvm::sys::path::filename(objectFilename).str() << "\n";

	for (Module::iterator fnIt = m->begin(), fn_ie = m->end(); fnIt != fn_ie;
	     ++fnIt) {
		if (!fnIt->isDeclaration()) {
			// Always try to write the filename before the function name, as otherwise
			// KCachegrind can create two entries for the function, one with an
			// unnamed file and one without.
			Function *fn = &*fnIt;
			const FunctionInfo &ii = executor.kmodule->infos->getFunctionInfo(*fn);
			if (ii.file != sourceFile) {
				of << "fl=" << ii.file << "\n";
				sourceFile = ii.file;
			}

			of << "fn=" << fnIt->getName().str() << "\n";
			for (Function::iterator bbIt = fnIt->begin(), bb_ie = fnIt->end();
			     bbIt != bb_ie; ++bbIt) {
				for (BasicBlock::iterator it = bbIt->begin(), ie = bbIt->end();
				     it != ie; ++it) {
					Instruction *instr = &*it;
					const InstructionInfo &ii = executor.kmodule->infos->getInfo(*instr);
					unsigned index = ii.id;
					if (ii.file != sourceFile) {
						of << "fl=" << ii.file << "\n";
						sourceFile = ii.file;
					}
					of << ii.assemblyLine << " ";
					of << ii.line << " ";
					for (unsigned i = 0; i < nStats; i++)
						if (istatsMask.test(i))
							of << sm.getIndexedValue(sm.getStatistic(i), index) << " ";
					of << "\n";

					if (UseCallPaths &&
					    (isa<CallInst>(instr) || isa<InvokeInst>(instr))) {
						CallSiteSummaryTable::iterator it = callSiteStats.find(instr);
						if (it != callSiteStats.end()) {
							for (auto fit = it->second.begin(), fie = it->second.end();
							     fit != fie; ++fit) {
								const Function *f = fit->first;
								CallSiteInfo &csi = fit->second;
								const FunctionInfo &fii =
									executor.kmodule->infos->getFunctionInfo(*f);

								if (fii.file != "" && fii.file != sourceFile)
									of << "cfl=" << fii.file << "\n";
								of << "cfn=" << f->getName().str() << "\n";
								of << "calls=" << csi.count << " ";
								of << fii.assemblyLine << " ";
								of << fii.line << "\n";

								of << ii.assemblyLine << " ";
								of << ii.line << " ";
								for (unsigned i = 0; i < nStats; i++) {
									if (istatsMask.test(i)) {
										Statistic &s = sm.getStatistic(i);
										uint64_t value;

										// Hack, ignore things that don't make sense on
										// call paths.
										if (&s == &stats::uncoveredInstructions) {
											value = 0;
										} else {
											value = csi.statistics.getValue(s);
										}

										of << value << " ";
									}
								}
								of << "\n";
							}
						}
					}
				}
			}
		}
	}

	if (istatsMask.test(stats::states.getID()))
		updateStateStatistics((uint64_t)-1);

	// Clear then end of the file if necessary (no truncate op?).
	unsigned pos = of.tell();
	for (unsigned i = pos; i < istatsSize; ++i)
		of << '\n';

	of.flush();
}

///

typedef std::map<Instruction *, std::vector<Function *> > calltargets_ty;

static calltargets_ty callTargets;
static std::map<Function *, std::vector<Instruction *> > functionCallers;
static std::map<Function *, unsigned> functionShortestPath;

static std::vector<Instruction *> getSuccs(Instruction *i) {
	BasicBlock *bb = i->getParent();
	std::vector<Instruction *> res;

	if (i == bb->getTerminator()) {
		for (succ_iterator it = succ_begin(bb), ie = succ_end(bb); it != ie; ++it)
			res.push_back(&*(it->begin()));
	} else {
		res.push_back(&*(++(i->getIterator())));
	}

	return res;
}

uint64_t klee::computeMinDistToUncovered(const KInstruction *ki,
                                         uint64_t minDistAtRA) {
	StatisticManager &sm = *theStatisticManager;
	if (minDistAtRA == 0) { // unreachable on return, best is local
		return sm.getIndexedValue(stats::minDistToUncovered, ki->info->id);
	} else {
		uint64_t minDistLocal =
			sm.getIndexedValue(stats::minDistToUncovered, ki->info->id);
		uint64_t distToReturn =
			sm.getIndexedValue(stats::minDistToReturn, ki->info->id);

		if (distToReturn == 0) { // return unreachable, best is local
			return minDistLocal;
		} else if (!minDistLocal) { // no local reachable
			return distToReturn + minDistAtRA;
		} else {
			return std::min(minDistLocal, distToReturn + minDistAtRA);
		}
	}
}

void StatsTracker::computeReachableUncovered() {
	KModule *km = executor.kmodule.get();
	const auto m = km->module.get();
	static bool init = true;
	const InstructionInfoTable &infos = *km->infos;
	StatisticManager &sm = *theStatisticManager;

	if (init) {
		init = false;

		// Compute call targets. It would be nice to use alias information
		// instead of assuming all indirect calls hit all escaping
		// functions, eh?
		for (Module::iterator fnIt = m->begin(), fn_ie = m->end(); fnIt != fn_ie;
		     ++fnIt) {
			for (Function::iterator bbIt = fnIt->begin(), bb_ie = fnIt->end();
			     bbIt != bb_ie; ++bbIt) {
				for (BasicBlock::iterator it = bbIt->begin(), ie = bbIt->end();
				     it != ie; ++it) {
					Instruction *inst = &*it;
					if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
						CallSite cs(inst);
						if (isa<InlineAsm>(cs.getCalledValue())) {
							// We can never call through here so assume no targets
							// (which should be correct anyhow).
							callTargets.insert(
								std::make_pair(inst, std::vector<Function *>()));
						} else if (Function *target = getDirectCallTarget(
								   cs, /*moduleIsFullyLinked=*/ true)) {
							callTargets[inst].push_back(target);
						} else {
							callTargets[inst] = std::vector<Function *>(
								km->escapingFunctions.begin(), km->escapingFunctions.end());
						}
					}
				}
			}
		}

		// Compute function callers as reflexion of callTargets.
		for (calltargets_ty::iterator it = callTargets.begin(),
		     ie = callTargets.end();
		     it != ie; ++it)
			for (std::vector<Function *>::iterator fit = it->second.begin(),
			     fie = it->second.end();
			     fit != fie; ++fit)
				functionCallers[*fit].push_back(it->first);

		// Initialize minDistToReturn to shortest paths through
		// functions. 0 is unreachable.
		std::vector<Instruction *> instructions;
		for (Module::iterator fnIt = m->begin(), fn_ie = m->end(); fnIt != fn_ie;
		     ++fnIt) {
			Function *fn = &*fnIt;
			if (fnIt->isDeclaration()) {
				if (fnIt->doesNotReturn()) {
					functionShortestPath[fn] = 0;
				} else {
					functionShortestPath[fn] = 1; // whatever
				}
			} else {
				functionShortestPath[fn] = 0;
			}

			// Not sure if I should bother to preorder here. XXX I should.
			for (Function::iterator bbIt = fnIt->begin(), bb_ie = fnIt->end();
			     bbIt != bb_ie; ++bbIt) {
				for (BasicBlock::iterator it = bbIt->begin(), ie = bbIt->end();
				     it != ie; ++it) {
					Instruction *inst = &*it;
					instructions.push_back(inst);
					unsigned id = infos.getInfo(*inst).id;
					sm.setIndexedValue(stats::minDistToReturn, id, isa<ReturnInst>(inst));
				}
			}
		}

		std::reverse(instructions.begin(), instructions.end());

		// I'm so lazy it's not even worklisted.
		bool changed;
		do {
			changed = false;
			for (auto it = instructions.begin(), ie = instructions.end(); it != ie;
			     ++it) {
				Instruction *inst = *it;
				unsigned bestThrough = 0;

				if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
					std::vector<Function *> &targets = callTargets[inst];
					for (std::vector<Function *>::iterator fnIt = targets.begin(),
					     ie = targets.end();
					     fnIt != ie; ++fnIt) {
						uint64_t dist = functionShortestPath[*fnIt];
						if (dist) {
							dist = 1 + dist; // count instruction itself
							if (bestThrough == 0 || dist < bestThrough)
								bestThrough = dist;
						}
					}
				} else {
					bestThrough = 1;
				}

				if (bestThrough) {
					unsigned id = infos.getInfo(*(*it)).id;
					uint64_t best,
					         cur = best = sm.getIndexedValue(stats::minDistToReturn, id);
					std::vector<Instruction *> succs = getSuccs(*it);
					for (std::vector<Instruction *>::iterator it2 = succs.begin(),
					     ie = succs.end();
					     it2 != ie; ++it2) {
						uint64_t dist = sm.getIndexedValue(stats::minDistToReturn,
						                                   infos.getInfo(*(*it2)).id);
						if (dist) {
							uint64_t val = bestThrough + dist;
							if (best == 0 || val < best)
								best = val;
						}
					}
					// there's a corner case here when a function only includes a single
					// instruction (a ret). in that case, we MUST update
					// functionShortestPath, or it will remain 0 (erroneously indicating
					// that no return instructions are reachable)
					Function *f = inst->getParent()->getParent();
					if (best != cur || (inst == &*(f->begin()->begin()) &&
					                    functionShortestPath[f] != best)) {
						sm.setIndexedValue(stats::minDistToReturn, id, best);
						changed = true;

						// Update shortest path if this is the entry point.
						if (inst == &*(f->begin()->begin()))
							functionShortestPath[f] = best;
					}
				}
			}
		} while (changed);
	}

	// compute minDistToUncovered, 0 is unreachable
	std::vector<Instruction *> instructions;
	for (Module::iterator fnIt = m->begin(), fn_ie = m->end(); fnIt != fn_ie;
	     ++fnIt) {
		// Not sure if I should bother to preorder here.
		for (Function::iterator bbIt = fnIt->begin(), bb_ie = fnIt->end();
		     bbIt != bb_ie; ++bbIt) {
			for (BasicBlock::iterator it = bbIt->begin(), ie = bbIt->end(); it != ie;
			     ++it) {
				Instruction *inst = &*it;
				unsigned id = infos.getInfo(*inst).id;
				instructions.push_back(inst);
				sm.setIndexedValue(
					stats::minDistToUncovered, id,
					sm.getIndexedValue(stats::uncoveredInstructions, id));
			}
		}
	}

	std::reverse(instructions.begin(), instructions.end());

	// I'm so lazy it's not even worklisted.
	bool changed;
	do {
		changed = false;
		for (std::vector<Instruction *>::iterator it = instructions.begin(),
		     ie = instructions.end();
		     it != ie; ++it) {
			Instruction *inst = *it;
			uint64_t best, cur = best = sm.getIndexedValue(stats::minDistToUncovered,
			                                               infos.getInfo(*inst).id);
			unsigned bestThrough = 0;

			if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
				std::vector<Function *> &targets = callTargets[inst];
				for (std::vector<Function *>::iterator fnIt = targets.begin(),
				     ie = targets.end();
				     fnIt != ie; ++fnIt) {
					uint64_t dist = functionShortestPath[*fnIt];
					if (dist) {
						dist = 1 + dist; // count instruction itself
						if (bestThrough == 0 || dist < bestThrough)
							bestThrough = dist;
					}

					if (!(*fnIt)->isDeclaration()) {
						uint64_t calleeDist = sm.getIndexedValue(
							stats::minDistToUncovered, infos.getFunctionInfo(*(*fnIt)).id);
						if (calleeDist) {
							calleeDist = 1 + calleeDist; // count instruction itself
							if (best == 0 || calleeDist < best)
								best = calleeDist;
						}
					}
				}
			} else {
				bestThrough = 1;
			}

			if (bestThrough) {
				std::vector<Instruction *> succs = getSuccs(inst);
				for (std::vector<Instruction *>::iterator it2 = succs.begin(),
				     ie = succs.end();
				     it2 != ie; ++it2) {
					uint64_t dist = sm.getIndexedValue(stats::minDistToUncovered,
					                                   infos.getInfo(*(*it2)).id);
					if (dist) {
						uint64_t val = bestThrough + dist;
						if (best == 0 || val < best)
							best = val;
					}
				}
			}

			if (best != cur) {
				sm.setIndexedValue(stats::minDistToUncovered, infos.getInfo(*inst).id,
				                   best);
				changed = true;
			}
		}
	} while (changed);

	for (std::set<ExecutionState *>::iterator it = executor.states.begin(),
	     ie = executor.states.end();
	     it != ie; ++it) {
		ExecutionState *es = *it;
		uint64_t currentFrameMinDist = 0;
		for (ExecutionState::stack_ty::iterator sfIt = es->stack.begin(),
		     sf_ie = es->stack.end();
		     sfIt != sf_ie; ++sfIt) {
			ExecutionState::stack_ty::iterator next = sfIt + 1;
			KInstIterator kii;

			if (next == es->stack.end()) {
				kii = es->pc;
			} else {
				kii = next->caller;
				++kii;
			}

			sfIt->minDistToUncoveredOnReturn = currentFrameMinDist;

			currentFrameMinDist = computeMinDistToUncovered(kii, currentFrameMinDist);
		}
	}
}
