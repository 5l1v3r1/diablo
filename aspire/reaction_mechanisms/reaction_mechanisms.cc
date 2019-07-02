/* This research is supported by the European Union Seventh Framework Programme (FP7/2007-2013), project ASPIRE (Advanced  Software Protection: Integration, Research, and Exploitation), under grant agreement no. 609734; on-line at https://aspire-fp7.eu/. */

#include "reaction_mechanisms.h"
#include <self_debugging_json.h>

#define MAX_INSERTIONS_PER_FUNCTION 1
#define PERCENT_NORMAL 1
#define PERCENT_REGION_3RDPARTY 10
#define PERCENT_REGION 100

#define REACTION_PREFIX "DIABLO_REACTION_"

using namespace std;

static vector<t_symbol*> reaction_syms;

static LogFile* L_REACTIONS = NULL;

void AddReactions(t_object* obj)
{
  auto has_incoming_switch = [] (t_bbl *bbl) {
    t_cfg_edge *e;
    BBL_FOREACH_PRED_EDGE(bbl, e) {
      if (CFG_EDGE_CAT(e) == ET_SWITCH
          || CFG_EDGE_CAT(e) == ET_IPSWITCH)
        return true;
    }

    return false;
  };

  /* If there's no reaction mechanisms available, we can't add any */
  if (reaction_syms.empty())
    return;

  STATUS(START, ("Adding Reactions"));
  NewDiabloPhase("Reactions");

  t_const_string logging_reactions_filename = StringConcat2 (OutputFilename(), ".diablo.reactions.log");
  INIT_LOGGING(L_REACTIONS, logging_reactions_filename);

  t_cfg* cfg = OBJECT_CFG(obj);

  /* Determine all registers that are allowed to be live */
  t_regset max_live_regs;
  RegsetSetSingleton(max_live_regs, ARM_REG_R0);
  RegsetSetUnion(max_live_regs, CFG_DESCRIPTION(cfg)->callee_saved);
  RegsetSetIntersect(max_live_regs, RegsetUnion(CFG_DESCRIPTION(cfg)->int_registers, CFG_DESCRIPTION(cfg)->cond_registers));

  /* Initialize RNGs */
  t_randomnumbergenerator *rng_insertion = RNGCreateChild(RNGGetRootGenerator(), "reaction_mechanisms_insertion");
  RNGSetRange(rng_insertion, 1, 100);
  t_randomnumbergenerator *rng_fun = RNGCreateChild(RNGGetRootGenerator(), "reaction_mechanisms_fun");
  RNGSetRange(rng_fun, 0, reaction_syms.size() - 1);

  /* Get the functions for all reaction mechanisms */
  vector<t_function*> reaction_funs;
  for (auto reaction_sym : reaction_syms)
    reaction_funs.push_back(BBL_FUNCTION(T_BBL(SYMBOL_BASE(reaction_sym))));

  /* Walk over all the instructions in the CFG and probabilistically insert reaction mechanisms */
  BblMarkInit();
  t_function* fun;
  CFG_FOREACH_FUN(cfg, fun)
  {
    t_uint32 insertions = 0;
    t_bbl* bbl;
    FUNCTION_FOREACH_BBL(fun, bbl)
    {
      if (insertions >= MAX_INSERTIONS_PER_FUNCTION)
        break;

      /* Only look at BBLs we haven't considered yet */
      if (BblIsMarked(bbl))
        continue;

      /* Consider the BBL */
      BblMark(bbl);

      /* Don't add reaction mechanisms in regions protected with self-debugging */
      bool in_sd_region = false;
      Region* region;
      const SelfDebuggingAnnotationInfo* info;
      BBL_FOREACH_SELFDEBUGGING_REGION(bbl, region, info)
      {
        in_sd_region = true;
        break;
      }
      if (in_sd_region)
        continue;

      t_ins* ins;
      BBL_FOREACH_INS(bbl, ins)
      {
        /* Determine the live registers before the instruction */
        t_regset live_regs = InsRegsLiveBefore(ins);
        RegsetSetIntersect(live_regs, RegsetUnion(CFG_DESCRIPTION(cfg)->int_registers, CFG_DESCRIPTION(cfg)->cond_registers));

        /* Determine whether this would be a good place to insert a call */
        t_bool allowed = RegsetIsSubset(max_live_regs, live_regs);
        t_bool useful = RegsetIn(live_regs, ARM_REG_R0);

        if (allowed && useful)
        {
          /* We will add more reaction mechanisms in BBLs that are present in regions, as we assume these to be executed more often */
          t_uint32 threshold = PERCENT_NORMAL;
          if (BBL_REGIONS(bbl))
          {
            /* Determine whether the BBL's origin is a 3rd party library or not. This could be cleaner, but I don't know how */
            if (INS_SRC_FILE(ins) && StringPatternMatch("*/3rd_party/*", INS_SRC_FILE(ins)))
              threshold = PERCENT_REGION_3RDPARTY;
            else
              threshold = PERCENT_REGION;
          }

          /* The frequency with which we insert calls depends on whether the BBL is part of a region or not */
          if (RNGGenerate(rng_insertion) <= threshold)
          {
            DEBUG(("Threshold: %u. BBL: @eiB.\nInstruction: @I", threshold, bbl, ins));

            t_string xxx = StringIo("@I", ins);
            START_LOGGING_TRANSFORMATION(L_REACTIONS, "reactions \"%s\"", xxx);
            Free(xxx);

            /* */
            bool split_before = TRUE;
            if (BBL_NINS(bbl) == 1
                && CFG_DESCRIPTION(cfg)->InsIsUnconditionalJump(BBL_INS_FIRST(bbl))
                && has_incoming_switch(bbl))
              split_before = FALSE;

            t_bool isThumb = ArmBblIsThumb(bbl);

            /* Split the BBL before the instruction */
            t_bbl *call_site = NULL;
            t_bbl *return_site = NULL;
            if (split_before) {
              call_site = bbl;
              return_site = BblSplitBlock(bbl, ins, TRUE);
              BblMark(return_site);
            }
            else {
              DEBUG(("doing after branch @eiB", bbl));

              call_site = BblSplitBlockNoTestOnBranches(bbl, ins, FALSE);
              return_site = CFG_EDGE_TAIL(BBL_SUCC_FIRST(call_site));

              /* fallthrough edge to jump edge */
              CFG_EDGE_SET_CAT(BBL_SUCC_FIRST(bbl), ET_JUMP);
            }

            /* Choose a reaction mechanism by chance */
            t_function* reaction = reaction_funs[RNGGenerate(rng_fun)];

            /* Add a call to a reaction mechanism. We add the instruction, kill the fallthrough edge created by splitting
             * the BBL and add a genuine call edge to a reaction mechanism.
             */
            t_arm_ins* arm_ins;
            ArmMakeInsForBbl(CondBranchAndLink, Append, arm_ins, call_site, isThumb, ARM_CONDITION_AL);
            CfgEdgeKill(BBL_SUCC_FIRST(call_site));
            CfgEdgeCreateCall(cfg, call_site, FUNCTION_BBL_FIRST(reaction), return_site, FunctionGetExitBlock(reaction));

            /* Exit loop, we don't want to insert another call in the same BBL */
            insertions++;

            STOP_LOGGING_TRANSFORMATION(L_REACTIONS);

            break;
          }
        }
      }
    }
  }

  /* Cleanup */
  RNGDestroy(rng_insertion);
  RNGDestroy(rng_fun);

  STATUS(STOP, ("Adding Reactions"));

  FINI_LOGGING(L_REACTIONS);
}

void AddReactionForceReachables(const t_object* obj, std::vector<std::string>& reachable_vector)
{
  /* Go over all symbols */
  t_symbol* tmp;
  SYMBOL_TABLE_FOREACH_SYMBOL(OBJECT_SUB_SYMBOL_TABLE(obj), tmp)
  {
    /* If the name matches, make sure it will be force reachable and add the symbol to the reaction symbols */
    if (StringPatternMatch(REACTION_PREFIX "*", SYMBOL_NAME(tmp)))
    {
      reachable_vector.emplace_back(SYMBOL_NAME(tmp));
      reaction_syms.push_back(tmp);
    }
  }
}
