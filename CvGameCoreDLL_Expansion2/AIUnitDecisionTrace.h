#pragma once
#ifndef AI_UNIT_DECISION_TRACE_H
#define AI_UNIT_DECISION_TRACE_H
#include <string>
#include <vector>
class CvCity;
namespace AIUnitDecisionTrace {
 struct Component { std::string source; int value; Component(const char* s,int v):source(s),value(v) {} };
 typedef std::vector<Component> Components;
 enum { PROCESS_UNIT_PROXY = -2 }; // Diagnostic candidate context; never a native buildable.
 int CurrentDecision();
 void DecisionContext(const char*);
 void DecisionAction(const char*);
 void Duration(int,int,int);
 void ProductionEvent(const CvCity*,const char*,int,int,int = 0,const char* = "NONE",const char* = "APPLIED");
 // One stack-local final action outcome; no pending or cross-turn action state.
 class Action { public: Action(const CvCity*,int,int,int,const char*,const char*); ~Action(); void Success(const char* = 0); private: const CvCity* m_city; int m_decision,m_kind,m_object; const char* m_type; const char* m_currency; bool m_success,m_enabled; Action(const Action&); Action& operator=(const Action&); };
 bool Configured(); bool Enabled(); bool Active();
 void SetCandidate(int kind,int object=-1); void SetObject(int object);
 void Arithmetic(const char*,int,int,int,int,const char*,const char*,int,int,int);
 int NewFlavorState();
 void ConsumeFlavorState(int);
 void Flavor(int,int,int,int,int,const Components&,bool); void FlavorStatus(bool);
 void Event(const char*,const char*,int,int,int,int,int,int,const char* = "");
 void Weighted(const char*,int,int,int);
 class Scope {public:Scope(const char*,int,int);~Scope();private:bool m_root,m_active;int m_kind,m_object,m_player,m_city,m_previousDecision;bool m_previousInDecision;const char* m_caller;Scope(const Scope&);Scope& operator=(const Scope&);};

}
#define AI_UNIT_TRACE_ARITH(contract, site, target, operation, operand) do { \
 const int traceOperand = (operand); const int traceBefore = target; \
 target operation traceOperand; \
 if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Arithmetic(contract, \
 m_pCity->getOwner(), m_pCity->GetID(), eUnit, site, #target, #operation, traceBefore, traceOperand, target); \
} while (0)


#endif
