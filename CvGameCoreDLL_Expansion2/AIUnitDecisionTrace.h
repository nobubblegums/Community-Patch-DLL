#pragma once
#ifndef AI_UNIT_DECISION_TRACE_H
#define AI_UNIT_DECISION_TRACE_H
class CvCity;
namespace AIUnitDecisionTrace {
 enum { PROCESS_UNIT_PROXY = -2 }; // Diagnostic candidate context; never a native buildable.
 int CurrentDecision();
 void DecisionContext(const char*);
 void DecisionAction(const char*);
 void Duration(int,int,int);
 void ProductionEvent(const CvCity*,const char*,int,int,int = 0,const char* = "NONE",const char* = "APPLIED");
 // Disabled until Begin; preserves one action outcome across native early returns.
 class Action { public: Action(); ~Action(); void Begin(const CvCity*,int,int,int,bool = false,int = 0); void Success(bool = false); private: const CvCity* m_city; int m_decision,m_kind,m_object; const char* m_type; const char* m_currency; bool m_success,m_enabled; Action(const Action&); Action& operator=(const Action&); };
 bool Configured(); bool Enabled();
 void SetCandidate(int kind,int object=-1); void SetObject(int object);
 void Arithmetic(const char*,int,int,int,int,const char*,const char*,int,int,int);
 void ResetFlavorObservation();
 bool PlayerEnabled(int);
 void PlayerFlavorState(int,const char*,const char*,int);
 int CacheFlavorState(const CvCity*);
 void CityFlavorState(const CvCity*,const char*,const char*,int);
 void Reject(const char*,bool,int = 0);
 void Recommendation(int,int,int,int,int);
 void ConsumeFlavorState(int);
 void Event(const char*,const char*,int,int,int,int,int,int,const char* = "");
 void Weighted(const char*,int,int,int);
 // Disabled until Begin; keeps the original scope lifetime without disabled-path setup.
 class Scope {public:Scope();void Begin(const char*,int,int);~Scope();private:bool m_root,m_active;int m_kind,m_object,m_player,m_city,m_previousDecision;bool m_previousInDecision,m_previousRecommendation;const char* m_caller;Scope(const Scope&);Scope& operator=(const Scope&);};

}
// Passive observers: gameplay mutations are performed at the call site.
#define AI_UNIT_TRACE_ADD(contract, site, tracker, target) do { \
 AIUnitDecisionTrace::Arithmetic(contract, m_pCity->getOwner(), m_pCity->GetID(), eUnit, site, #target, "+=", tracker, (target) - (tracker), target); \
 tracker = target; \
} while (0)
#define AI_UNIT_TRACE_SUB(contract, site, tracker, target) do { \
 AIUnitDecisionTrace::Arithmetic(contract, m_pCity->getOwner(), m_pCity->GetID(), eUnit, site, #target, "-=", tracker, (tracker) - (target), target); \
 tracker = target; \
} while (0)
#define AI_UNIT_TRACE_ASSIGN(contract, site, tracker, target) do { \
 AIUnitDecisionTrace::Arithmetic(contract, m_pCity->getOwner(), m_pCity->GetID(), eUnit, site, #target, "=", tracker, target, target); \
 tracker = target; \
} while (0)
#define AI_UNIT_TRACE_DIV(contract, site, tracker, target) do { \
 AIUnitDecisionTrace::Arithmetic(contract, m_pCity->getOwner(), m_pCity->GetID(), eUnit, site, #target, "/=", tracker, 5, target); \
 tracker = target; \
} while (0)


#endif
