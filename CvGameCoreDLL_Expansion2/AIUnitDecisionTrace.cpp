// Production-AI decision diagnostics; SQLite backend remains in SqliteLogger.cpp.

#include "CvGameCoreDLLPCH.h"
#include "AIUnitDecisionTrace.h"
#include "SqliteLogger.h"
#include "CustomMods.h"
#include "CvFlavorManager.h"
#include "CvEconomicAI.h"
#include "CvMilitaryAI.h"
#include "CvGrandStrategyAI.h"
#include "CvCityStrategyAI.h"
#include "CvCitySpecializationAI.h"

#include <vector>
#include <map>
#include <string.h>

// must be included after all other headers
#include "LintFree.h"

namespace AIUnitDecisionTrace {
	namespace {
		int nextDecision=0, decision=0, ordinal=0, player=-1, city=-1, kind=-1, object=-1;
		int nextFlavorState=0, flavorState=-1;
		int nextObservation=0;
		CvString decisionType, decisionTrigger, selectedAction;
		CvString productionBeforeObjectType, productionAfterObjectType, orderOutcome, winnerKindName, winnerObjectType;
		bool consumedFlavor=false, observedAction=false;
		bool conflictingFlavorStates=false;
		bool recommendation=false;
		bool optionsLoaded=false, majorsOnly=true, filters=false, terms=true, flavors=true;
		void Options() {
			if (optionsLoaded) return;
			majorsOnly=gCustomMods.getOption("AI_UNIT_DECISION_LOGGING_MAJORS_ONLY",1)!=0;
			filters=gCustomMods.getOption("AI_UNIT_DECISION_LOGGING_FILTERS",0)!=0;
			terms=gCustomMods.getOption("AI_UNIT_DECISION_LOGGING_TERMS",1)!=0;
			flavors=gCustomMods.getOption("AI_UNIT_DECISION_LOGGING_FLAVORS",1)!=0;
			optionsLoaded=true;
		}
		// Provisional unit arithmetic holds only literal labels and native values.
		// SQL names/strings are resolved after disposition, within this decision.
		struct PendingTerm {
			const char *id,*target,*op,*event,*reason;
			int before,operand,after,sequence,kind,object,player,city,parentObject;
			PendingTerm(const char* i,const char* t,const char* o,int b,int v,int a,const char* e,const char* r,int seq,int k,int obj,int p,int c):id(i),target(t),op(o),event(e),reason(r),before(b),operand(v),after(a),sequence(seq),kind(k),object(obj),player(p),city(c),parentObject(-1) {}
		};
		bool registered=false;
		bool inDecision=false;
		struct Candidate {
			int player,city,kind,object,base,sanity,finalWeight,turns,eligible,selected,pool; bool hasBase,hasScore;
			std::vector<PendingTerm> pending;
			CvString reason;
			Candidate():player(-1),city(-1),kind(-1),object(-1),base(0),sanity(0),finalWeight(0),turns(-1),eligible(-1),selected(0),pool(-1),hasBase(false),hasScore(false){
			}
		};
		typedef std::pair<int,int> Key;
		std::map<Key,Candidate> candidates;
		CvString selectionMode;
		// One BatchWriter per telemetry table for the current GameId/Turn. BatchWriter still
		// auto-flushes at SQLITE_LOGGING_BATCHED_BUFFER_ROWS_MAX, bounding memory usage.
		int batchGameId=-1, batchTurn=-1;
		SqliteLogger::BatchWriter* termBatch=NULL;
		SqliteLogger::BatchWriter* candidateBatch=NULL;
		SqliteLogger::BatchWriter* decisionBatch=NULL;
		SqliteLogger::BatchWriter* playerFlavorBatch=NULL;
		SqliteLogger::BatchWriter* cityFlavorBatch=NULL;
		SqliteLogger::BatchWriter* eventBatch=NULL;
		SqliteLogger::BatchWriter* filterBatch=NULL;
		std::vector<Key> postPool;
		const char* termNames[]={
			"","FLAVOR_CACHE","CACHED_WEIGHT","ROLE_RECOMMENDATION","RECOMMEND_DURATION","","PURCHASE_AND_CITY","CITY_MATURITY","MAINTENANCE","PLACEMENT","COUNTS","FORCE_LIMIT","MINOR_FORCE","LANDMASS","RESOURCE_REQUIRED","RESOURCE_SURPLUS","SPECIAL_ROLE","CITY_SUPPORT","SUICIDE_NUKE","PURCHASE_SURPLUS","EXPLORATION_CARRIER","NAVAL_DEFICIT","LAND_FORCE_DEFICIT","AIR_BALANCE","ANTI_AIR","CONQUEST","DIPLOMAT","SPACESHIP","RELIGION","TRADE","TRAIT","SETTLER","ARCHAEOLOGIST","WORKBOAT","WORKER","WAR_PRESSURE","BEST_CITY","PROMOTION","OPERATION_ROLE","DOMAIN_COUNTS","CONTINUATION","COMPLETION_FREE","ROLE_BALANCE","FINAL"
		};
		const char* Name(const char* contract) {
			int i=contract && contract[0]=='t'?atoi(contract+1):0;
			return i>0 && i<=43?termNames[i]:contract;
		} const char* KindName(int k) {
			if(k==CITY_BUILDABLE_UNIT) return "UNIT";
			if(k==CITY_BUILDABLE_UNIT_FOR_OPERATION) return "UNIT_FOR_OPERATION";
			if(k==CITY_BUILDABLE_UNIT_FOR_ARMY) return "UNIT_FOR_ARMY";
			if(k==PROCESS_UNIT_PROXY) return "PROCESS_UNIT_PROXY";
			if(k==CITY_BUILDABLE_BUILDING) return "BUILDING";
			if(k==CITY_BUILDABLE_PROJECT) return "PROJECT";
			if(k==CITY_BUILDABLE_PROCESS) return "PROCESS";
			return "UNKNOWN";
		} int OrderKind(int order) {
			return order==ORDER_TRAIN ? CITY_BUILDABLE_UNIT : order==ORDER_CONSTRUCT ? CITY_BUILDABLE_BUILDING : order==ORDER_CREATE ? CITY_BUILDABLE_PROJECT : order==ORDER_MAINTAIN ? CITY_BUILDABLE_PROCESS : -1;
		} CvString Symbol(int k,int obj) {
			if(obj<0)return "UNKNOWN";
			CvBaseInfo* info=NULL;
			if(k==PROCESS_UNIT_PROXY || k==CITY_BUILDABLE_UNIT || k==CITY_BUILDABLE_UNIT_FOR_OPERATION || k==CITY_BUILDABLE_UNIT_FOR_ARMY)info=GC.getUnitInfo((UnitTypes)obj);
			else if(k==CITY_BUILDABLE_BUILDING)info=GC.getBuildingInfo((BuildingTypes)obj);
			else if(k==CITY_BUILDABLE_PROCESS)info=GC.getProcessInfo((ProcessTypes)obj);
			else if(k==CITY_BUILDABLE_PROJECT)info=GC.getProjectInfo((ProjectTypes)obj);
			return info?info->GetType():"UNKNOWN";
		}
		void Register(const char* name,const char* const* columns,int count) {
			TableDef schema;
			for(int i=0;i<count;++i)
				schema.push_back(ColumnDef(columns[i]+2, columns[i][0]=='I' ? Database::COLTYPE_INT : Database::COLTYPE_TEXT));
			GET_SQLITE_LOGGER().RegisterTable(name,schema,true);
		}
		CvString PlayerName(int p) {
			return p>=0 && p<MAX_PLAYERS ? GET_PLAYER((PlayerTypes)p).getCivilizationShortDescription() : "UNKNOWN";
		}
		CvString CityName(int p,int c) {
			if(p<0 || p>=MAX_PLAYERS) return "UNKNOWN";
			CvCity* pCity=GET_PLAYER((PlayerTypes)p).getCity(c);
			if(pCity) return pCity->getName();
			return "UNKNOWN";
		}
		void Ensure() {
			if(registered)return;
			const char* d[]={"I:ObservationID","I:DecisionID","T:Player","T:City","I:FlavorStateID","T:DecisionType","T:DecisionTrigger","T:SelectionMode","T:WinnerKindName","T:WinnerObjectType","T:ProductionBeforeObjectType","T:ProductionAfterObjectType","T:OrderOutcome","T:ProposedAction"};
			Register("AIProductionDecision",d,14);
			const char* c[]={"I:DecisionID","T:Player","T:City","T:BuildableKindName","T:ObjectType","I:InitialWeight","I:SanityWeight","I:FinalWeight","I:EstimatedTurnsLeft","I:PoolIndex","I:Selected","T:Reason"};
			Register("AIProductionCandidate",c,12);
			const char* t[]={"I:DecisionID","I:Sequence","T:Player","T:City","T:BuildableKindName","T:ObjectType","I:BeforeValue","I:Operand","I:AfterValue","T:Step","T:Target","T:VariableName","T:Operation","T:Reason","T:ParentObjectType"};
			Register("AIProductionTerm",t,15);
			const char* f[]={"I:StateID","I:PlayerID","T:EventType","T:Subject","I:Value"};
			Register("AIPlayerFlavorState",f,5);
			const char* fc[]={"I:StateID","I:PlayerID","I:CityID","T:EventType","T:Subject","I:Value"};
			Register("AICityFlavorState",fc,6);
			const char* filter[]={"I:DecisionID","T:Player","T:City","T:BuildableKindName","T:ObjectType","T:Rule"};
			Register("AIProductionFilter",filter,6);
			const char* e[]={"I:ObservationID","I:DecisionID","T:Player","T:City","T:EventType","T:BuildableKindName","T:ObjectType","T:Currency","T:Result"};
			Register("AIProductionEvent",e,9);
			registered=true;
		}
		void FlushBatch(SqliteLogger::BatchWriter*& batch) {
			if (!batch) return;
			batch->flush();
			delete batch;
			batch=NULL;
		}
		void FlushTurnBatches() {
			FlushBatch(termBatch);
			FlushBatch(candidateBatch);
			FlushBatch(decisionBatch);
			FlushBatch(playerFlavorBatch);
			FlushBatch(cityFlavorBatch);
			FlushBatch(eventBatch);
			FlushBatch(filterBatch);
			batchGameId=-1;
			batchTurn=-1;
		}
		SqliteLogger::BatchWriter* TurnBatch(SqliteLogger::BatchWriter*& batch,const char* table) {
			Ensure();
			const int gameId=GC.getGame().getGameDatabaseId();
			const int turn=GC.getGame().getElapsedGameTurns();
			if (batchGameId!=gameId || batchTurn!=turn) {
				// Never let a BatchWriter cross the GameId/Turn metadata captured at creation.
				FlushTurnBatches();
				batchGameId=gameId;
				batchTurn=turn;
			}
			if (!batch) batch=new SqliteLogger::BatchWriter(GET_SQLITE_LOGGER().BeginLogBatch(table));
			return batch;
		}
		void WriteTerm(const char* id,const char* target,const char* op,int before,int operand,int after,int /*site*/,const char* event,const char* reason,int parentObject=-1) {
			if(!Enabled())return;
			Ensure();
			const char* variable="";
			const char* semanticTarget=target;
			if (!strcmp(target,"iBonus")) { variable=target; semanticTarget="SANITY_BONUS"; }
			else if (!strcmp(target,"iFlavorWeight")) {
				variable=target;
				semanticTarget="SANITY_SCORE";
			}
			else if (!strcmp(target,"sanity")) semanticTarget="SANITY_SCORE";
			else if (!strcmp(target,"recommendation")) semanticTarget="RECOMMENDATION_WEIGHT";
			else if (!strcmp(target,"competition")) semanticTarget="COMPETITION_WEIGHT";
			else if (!strcmp(target,"selection")) semanticTarget="SELECTION";
			if (!strcmp(op,"DURATION") && !strcmp(target,"competition")) variable="iTurns";
			const char* step=Name(id);
			if (!strcmp(op,"DRAW")) step=id;
			else if (!strcmp(op,"RECOMMENDED")) step="ROLE_RECOMMENDED";
			else if (!id[0] && !strcmp(op,"DURATION")) step="DURATION_REWEIGHTING";
			else if (!id[0]) step=event;
			TurnBatch(termBatch,"AIProductionTerm")->BeginLogRow().bind(decision).bind(ordinal).bind(PlayerName(player).c_str()).bind(CityName(player,city).c_str()).bind(recommendation ? "ROLE_RECOMMENDATION" : KindName(kind)).bind(Symbol(kind,object).c_str()).bind(before).bind(operand).bind(after).bind(step).bind(semanticTarget).bind(variable).bind(op).bind(reason).bind(parentObject>=0 ? Symbol(CITY_BUILDABLE_PROCESS,parentObject).c_str() : "").addRowToBatch();
		}
		void Term(const char* id,const char* target,const char* op,int before,int operand,int after,int site,const char* event,const char* reason) {
			if (!Enabled() || !terms) return;
			++ordinal;
			if (!strcmp(target,"selection") || recommendation) {
				WriteTerm(id,target,op,before,operand,after,site,event,reason);
				return;
			}
			candidates[Key(kind,object)].pending.push_back(PendingTerm(id,target,op,before,operand,after,event,reason,ordinal,kind,object,player,city));
		}

	}
	bool PlayerEnabled(int p) {
		if (!Configured() || p<0 || p>=MAX_PLAYERS) return false;
		Options();
		return !majorsOnly || GET_PLAYER((PlayerTypes)p).isMajorCiv();
	}
	namespace {
		// Latest logged sparse state, independent of when native caches rebuild.
		std::map<int,int> observedPlayers;
		std::map<Key,int> observedCities;
		void PlayerStateRow(int p,const char* event,const char* subject,int value) {
			const int state=++nextFlavorState;
			// Grand Strategy is read directly by scoring, not by the city flavor cache.
			if (strcmp(event,"GRAND_STRATEGY")) observedPlayers[p]=state;
			TurnBatch(playerFlavorBatch,"AIPlayerFlavorState")->BeginLogRow().bind(state).bind(p).bind(event).bind(subject).bind(value).addRowToBatch();
		}
		void CityStateRow(const CvCity* c,const char* event,const char* subject,int value) {
			const int state=observedCities[Key(c->getOwner(),c->GetID())]=++nextFlavorState;
			TurnBatch(cityFlavorBatch,"AICityFlavorState")->BeginLogRow().bind(state).bind(c->getOwner()).bind(c->GetID()).bind(event).bind(subject).bind(value).addRowToBatch();
		}
		bool FlavorReady(int p) {
			if (!PlayerEnabled(p) || !flavors || GC.getGame().getGameDatabaseId()<=0 || !GET_SQLITE_LOGGER().IsEnabled()) return false;
			Ensure();
			return true;
		}
		void ObservePlayer(int p) {
			if (!observedPlayers.insert(std::make_pair(p,-1)).second) return;
			CvPlayer& owner=GET_PLAYER((PlayerTypes)p);
			PlayerStateRow(p,"OBSERVED","",0);
			for (int f=0; f<GC.getNumFlavorTypes(); ++f)
				PlayerStateRow(p,"BASE",GC.getFlavorTypes((FlavorTypes)f).c_str(),owner.GetFlavorManager()->GetPersonalityIndividualFlavor((FlavorTypes)f));
			CvEconomicAI* economic=owner.GetEconomicAI();
			for (int i=0; i<economic->GetEconomicAIStrategies()->GetNumEconomicAIStrategies(); ++i)
				if (economic->IsUsingStrategy((EconomicAIStrategyTypes)i)) PlayerStateRow(p,"ECONOMIC_STRATEGY",economic->GetEconomicAIStrategies()->GetEntry(i)->GetType(),1);
			CvMilitaryAI* military=owner.GetMilitaryAI();
			for (int i=0; i<military->GetMilitaryAIStrategies()->GetNumMilitaryAIStrategies(); ++i)
				if (military->IsUsingStrategy((MilitaryAIStrategyTypes)i)) PlayerStateRow(p,"MILITARY_STRATEGY",military->GetMilitaryAIStrategies()->GetEntry(i)->GetType(),1);
			CvGrandStrategyAI* grand=owner.GetGrandStrategyAI();
			const AIGrandStrategyTypes active=grand->GetActiveGrandStrategy();
			PlayerStateRow(p,"GRAND_STRATEGY",active==NO_AIGRANDSTRATEGY ? "NONE" : grand->GetAIGrandStrategies()->GetEntry(active)->GetType(),1);
		}
		void ObserveCity(const CvCity* c) {
			if (observedCities.insert(std::make_pair(Key(c->getOwner(),c->GetID()),-1)).second) {
				CvCityStrategyAI* strategy=c->GetCityStrategyAI();
				CityStateRow(c,"OBSERVED","",0);
				for (int i=0; i<strategy->GetAICityStrategies()->GetNumAICityStrategies(); ++i)
					if (strategy->IsUsingCityStrategy((AICityStrategyTypes)i)) CityStateRow(c,"CITY_STRATEGY",strategy->GetAICityStrategies()->GetEntry(i)->GetType(),1);
				const CitySpecializationTypes specialization=strategy->GetSpecialization();
				CityStateRow(c,"SPECIALIZATION",specialization==NO_CITY_SPECIALIZATION ? "NONE" : GC.getCitySpecializationInfo(specialization)->GetType(),1);
			}
		}
	}
	// Load restores native caches independently: their IDs remain -1 until a real rebuild.
	void ResetFlavorObservation() {
		// resolveGameDatabaseId() calls this before replacing the GameId, so preserve the
		// pending tail from the previous game/load before invalidating sparse flavor state.
		FlushTurnBatches();
		observedPlayers.clear(); observedCities.clear();
	}
	void PlayerFlavorState(int p,const char* event,const char* subject,int value) {
		if (!FlavorReady(p)) return;
		if (!strcmp(event,"BASE_ADJUSTMENT") && !observedPlayers.count(p)) return;
		ObservePlayer(p);
		PlayerStateRow(p,event,subject,value);
	}
	void CityFlavorState(const CvCity* c,const char* event,const char* subject,int value) {
		if (!c || !FlavorReady(c->getOwner())) return;
		ObservePlayer(c->getOwner());
		ObserveCity(c);
		CityStateRow(c,event,subject,value);
	}
	int CacheFlavorState(const CvCity* c) {
		if (!c || !FlavorReady(c->getOwner())) return -1;
		ObservePlayer(c->getOwner());
		ObserveCity(c);
		// Freeze the relevant sparse-state cutoff only at actual rebuild completion.
		return max(observedPlayers[c->getOwner()],observedCities[Key(c->getOwner(),c->GetID())]);
	}

	int CurrentDecision() { return Enabled() ? decision : 0; }
	void DecisionContext(const char* trigger) {
		if (!Enabled()) return;
		decisionTrigger=trigger;
		if (!strcmp(trigger,"TURN_RECONSIDERATION")) {
			const CvCity* c=GET_PLAYER((PlayerTypes)player).getCity(city);
			if (c) decisionTrigger=!c->isProduction() ? "TURN_NO_PRODUCTION" : c->isProductionProcess() ? "TURN_PROCESS_RECONSIDERATION" : "TURN_DIRTY_RECONSIDERATION";
		}
	}
	void DecisionAction(const char* action) { if (Enabled()) selectedAction=action; }
	void Duration(int before,int turns,int after) {
		if (!Enabled() || recommendation) return;
		Candidate& c=candidates[Key(kind,object)]; c.hasScore=true;
		c.sanity=before; c.turns=turns;
		c.player=player; c.city=city; c.kind=kind; c.object=object;
		if (!c.hasBase) { c.base=before; c.hasBase=true; }
		c.finalWeight=after;
		// Hurry discounts the raw seed before sanity; retain this boundary in one term.
		if (decisionType=="ChooseHurry" && before!=after) Term("","competition","DURATION",before,turns,after,0,"APPLIED","");
		if (decisionType=="ChooseHurry") c.hasScore=false;
	}
	// Only pushOrder reports queue application; Hurry investment does not insert an order.
	void ProductionOrderResult(const CvCity* c,int origin,const char* outcome) {
		if (!c || !Enabled() || origin!=decision || player!=c->getOwner() || city!=c->GetID() || decisionType!="ChooseProduction") return;
		const OrderData* head=c->headOrderQueueNode();
		productionAfterObjectType=head ? Symbol(OrderKind(head->eOrderType),head->iData1) : "UNKNOWN";
		orderOutcome=outcome;
		observedAction=true;
	}
	void ProductionEvent(const CvCity* c,const char* type,int order,int obj,int origin,const char* currency,const char* result) {
		if (!c || !PlayerEnabled(c->getOwner()) || obj<0 || !GET_SQLITE_LOGGER().IsEnabled()) return;
		Ensure();
		const int k=OrderKind(order);
		if (Enabled() && origin==decision && player==c->getOwner() && city==c->GetID()) observedAction=true;
		TurnBatch(eventBatch,"AIProductionEvent")->BeginLogRow().bind(++nextObservation).bind(origin).bind(PlayerName(c->getOwner()).c_str()).bind(c->getName().c_str()).bind(type).bind(KindName(k)).bind(Symbol(k,obj).c_str()).bind(currency).bind(result).addRowToBatch();
	}
	Action::Action():m_city(NULL),m_decision(0),m_kind(-1),m_object(-1),m_type(NULL),m_currency(NULL),m_success(false),m_enabled(false) {}
	void Action::Begin(const CvCity* c,int order,int obj,int yield,bool investment,int origin) {
		// Shared purchase dispatchers also call these APIs with NO_UNIT/NO_BUILDING.
		if (!c || !PlayerEnabled(c->getOwner()) || obj<0 || !GET_SQLITE_LOGGER().IsEnabled()) return;
		m_city=c; m_decision=origin; m_kind=order; m_object=obj;
		m_currency=yield==YIELD_GOLD ? "GOLD" : yield==YIELD_FAITH ? "FAITH" : yield==NO_YIELD && investment ? "NONE" : "UNKNOWN";
		if (order==ORDER_CONSTRUCT) investment=investment || (yield==YIELD_GOLD && MOD_BALANCE_BUILDING_INVESTMENTS);
		m_type=order==ORDER_TRAIN ? (investment ? "UNIT_INVESTMENT" : "UNIT_PURCHASE") : (investment ? "BUILDING_INVESTMENT" : "BUILDING_PURCHASE");
		m_enabled=true;
	}
	void Action::Success(bool investment) {
		m_success=true;
		if (investment) m_type=m_kind==ORDER_TRAIN ? "UNIT_INVESTMENT" : "BUILDING_INVESTMENT";
	}
	Action::~Action() {
		if (m_enabled) ProductionEvent(m_city,m_type,m_kind,m_object,m_decision,m_currency,m_success ? "APPLIED" : "FAILED");
	}
	bool Configured(){
		return MOD_AI_UNIT_DECISION_LOGGING;
	}
	bool Enabled(){
		return Configured() && inDecision && decision!=0;
	}
	void SetCandidate(int k,int obj){
		if(!Enabled())return;
		kind=k;
		object=obj;

	}
	void SetObject(int obj){
		if(Enabled())object=obj;
	}
	Scope::Scope():m_root(false),m_active(false) {}
	void Scope::Begin(const char* caller,int p,int c) {
		m_kind=kind; m_object=object; m_player=player; m_city=city;
		m_previousDecision=decision; m_previousInDecision=inDecision; m_previousRecommendation=recommendation;
		if(!Configured() || !PlayerEnabled(p))return;

		if(!GET_SQLITE_LOGGER().IsEnabled())return;

		if(!inDecision && (strcmp(caller,"ChooseProduction")==0 || strcmp(caller,"ChooseHurry")==0)) {
			m_root=true;
			inDecision=true;

			Ensure();
			if (FlavorReady(p)) ObservePlayer(p);

			decision=++nextDecision;
			decisionType=caller;
			decisionTrigger="UNKNOWN";
			selectedAction="NONE";
			productionBeforeObjectType=productionAfterObjectType="UNKNOWN";
			orderOutcome="NONE";
			winnerKindName=winnerObjectType="UNKNOWN";
			consumedFlavor=observedAction=false;
			const CvCity* traceCity=GET_PLAYER((PlayerTypes)p).getCity(c);
			const OrderData* traceHead=traceCity ? traceCity->headOrderQueueNode() : NULL;
			if (traceHead) productionBeforeObjectType=Symbol(OrderKind(traceHead->eOrderType),traceHead->iData1);

			ordinal=0;
			flavorState=-1;
			conflictingFlavorStates=false;
			candidates.clear();
			selectionMode="NO_CHOICE";
			postPool.clear();
		}    m_active=decision!=0;
		if(m_active){
			player=p;
			city=c;
			m_caller=caller;
			if (!strcmp(caller,"RecommendUnit")) recommendation=true;
			// A process can evaluate a free unit using a fixed sanity input, not its own process identity.
			if (!strcmp(caller,"CheckUnitBuildSanity") && kind==CITY_BUILDABLE_PROCESS) kind=PROCESS_UNIT_PROXY;
		}
	}
	Scope::~Scope(){
		if(!m_active)return;
		if(m_root){
			const int savedKind=kind, savedObject=object, savedOrdinal=ordinal, savedPlayer=player, savedCity=city;
			for (std::map<Key,Candidate>::iterator it=candidates.begin(); it!=candidates.end();) {
				const Candidate& c=it->second;
				if (!c.hasScore && c.pool<0 && !c.selected) { candidates.erase(it++); continue; }
				for (size_t i=0; i<c.pending.size(); ++i) {
					const PendingTerm& t=c.pending[i];
					kind=t.kind; object=t.object; ordinal=t.sequence; player=t.player; city=t.city;
					WriteTerm(t.id,t.target,t.op,t.before,t.operand,t.after,0,t.event,t.reason,t.parentObject);
				}
				++it;
			}
			kind=savedKind; object=savedObject; ordinal=savedOrdinal; player=savedPlayer; city=savedCity;
			if (!candidates.empty() || ordinal>0 || consumedFlavor || observedAction || selectedAction!="NONE") {
				SqliteLogger::BatchWriter* kCandidateBatch=TurnBatch(candidateBatch,"AIProductionCandidate");
				for(std::map<Key,Candidate>::const_iterator it=candidates.begin();it!=candidates.end();++it){
					const Candidate& c=it->second;
					CvString symbol=Symbol(c.kind,c.object);
					CvString playerName=PlayerName(c.player), cityName=CityName(c.player,c.city);
					kCandidateBatch->BeginLogRow().bind(decision).bind(playerName.c_str()).bind(cityName.c_str()).bind(KindName(c.kind)).bind(symbol.c_str()).bind(c.base).bind(c.sanity).bind(c.finalWeight).bind(c.turns).bind(c.eligible==1 ? c.pool : -1).bind(c.selected).bind(c.reason.c_str()).addRowToBatch();
				}

				// ObservationID preserves semantic ordering; physical table commits are turn-batched.
				TurnBatch(decisionBatch,"AIProductionDecision")->BeginLogRow().bind(++nextObservation).bind(decision).bind(PlayerName(player).c_str()).bind(CityName(player,city).c_str()).bind(conflictingFlavorStates ? -1 : flavorState).bind(m_caller).bind(decisionTrigger.c_str()).bind(selectionMode.c_str()).bind(winnerKindName.c_str()).bind(winnerObjectType.c_str()).bind(productionBeforeObjectType.c_str()).bind(productionAfterObjectType.c_str()).bind(orderOutcome.c_str()).bind(selectedAction.c_str()).addRowToBatch();
			}
			candidates.clear();
		} else if (kind==PROCESS_UNIT_PROXY) {
			// Keep causal unit arithmetic with its enclosing process, never as a competitor.
			for (std::map<Key,Candidate>::iterator it=candidates.begin(); it!=candidates.end();) {
				if (it->first.first!=PROCESS_UNIT_PROXY) { ++it; continue; }
				Candidate& parent=candidates[Key(m_kind,m_object)];
				for (size_t i=0; i<it->second.pending.size(); ++i) it->second.pending[i].parentObject=m_object;
				parent.pending.insert(parent.pending.end(),it->second.pending.begin(),it->second.pending.end());
				candidates.erase(it++);
			}
		}decision=m_previousDecision;
		inDecision=m_previousInDecision;
		recommendation=m_previousRecommendation;
		kind=m_kind;
		object=m_object;
		player=m_player;
		city=m_city;
	}
	void Arithmetic(const char* id,int,int,int unit,int site,const char* target,const char* op,int before,int operand,int after){
		if(!Enabled() || recommendation || !terms)return;
		object=unit;
		if(strcmp(target,"iBonus") && strcmp(target,"iFlavorWeight"))return;
		if (before==after) return;
		const char* operation = !strcmp(op,"+=")?"ADD":!strcmp(op,"-=")?"SUBTRACT":!strcmp(op,"*=")?"MULTIPLY":!strcmp(op,"/=")?"DIVIDE":"ASSIGN";
		const char* strategy="";
		if (!strcmp(id,"t042") && site>=124 && site<=132) {
			const char* names[]={"MILITARYAISTRATEGY_NEED_ARCHER","MILITARYAISTRATEGY_NEED_SIEGE","MILITARYAISTRATEGY_NEED_SKIRMISHER","MILITARYAISTRATEGY_NEED_MOBILE","MILITARYAISTRATEGY_NEED_NAVAL_MELEE","MILITARYAISTRATEGY_NEED_NAVAL_RANGED","MILITARYAISTRATEGY_NEED_SUBMARINE","MILITARYAISTRATEGY_NEED_BOMBER","MILITARYAISTRATEGY_NEED_FIGHTER"};
			strategy=names[site-124];
		}
		Term(id,target,operation,before,operand,after,site,"APPLIED",strategy);
	}
	void ConsumeFlavorState(int state) {
		if (!Enabled() || recommendation || state<0) return;
		consumedFlavor=true;
		if (flavorState<0 && !conflictingFlavorStates) flavorState=state;
		else if (flavorState!=state) { flavorState=-1; conflictingFlavorStates=true; }
	}
	void Reject(const char* rule,bool scoreCaused,int score) {
		if (!Enabled() || recommendation) return;
		Candidate& c=candidates[Key(kind,object)];
		c.reason=rule; c.hasScore=scoreCaused; c.finalWeight=0;
		if (scoreCaused) c.sanity=SR_UNKNOWN;
		c.player=player; c.city=city; c.kind=kind; c.object=object;
		if (scoreCaused) Term("t043","sanity","REJECT",score,0,SR_UNKNOWN,0,"REJECTED",rule);
		else {
			c.pending.clear();
			if (filters) TurnBatch(filterBatch,"AIProductionFilter")->BeginLogRow().bind(decision).bind(PlayerName(player).c_str()).bind(CityName(player,city).c_str()).bind(KindName(kind)).bind(Symbol(kind,object).c_str()).bind(rule).addRowToBatch();
		}
	}
	void Recommendation(int unit,int role,int before,int turns,int after) {
		if (!Enabled() || !terms || !recommendation) return;
		object=unit;
		CvString detail; detail.Format("role=%d",role);
		Term("t004","recommendation","DURATION",before,turns,after,0,"APPLIED",detail.c_str());
	}
	void Event(const char* event,const char* id,int,int,int k,int obj,int before,int after,const char* field){
		if(!Enabled())return;
		if (recommendation) {
			if (!strcmp(event,"ROLE_RECOMMENDED")) { object=obj; Term("t004","recommendation","RECOMMENDED",before,0,after,0,"RECOMMENDED",""); }
			return;
		}
		if(strcmp(event,"HURRY_NO_CHOICE")==0) return;
		if(k==-1)k=kind;
		if(obj<0 && (strcmp(event,"DEMAND_RECOMMENDATION")==0 || strcmp(event,"HURRY_DEMAND")==0)){
			return;
		}
		if(obj<0)obj=object;
		if(obj<0)return;
		kind=k;
		object=obj;
		Candidate& c=candidates[Key(k,obj)];
		if(strcmp(event,"CACHED_WEIGHT")==0) { c.base=before; c.hasBase=true; }
		if(strcmp(event,"NONFLAVOR_BASE")==0) { c.base=before; c.hasBase=true; }
		if(strcmp(event,"INPUT")==0 && strcmp(id,"t005")==0){if(!c.hasBase) { c.base=before; c.hasBase=true; }}
		c.player=player;
		c.city=city;
		c.kind=k;
		c.object=obj;
		if((strcmp(event,"PRE")==0 && !c.hasBase) || strcmp(event,"BASE_WEIGHT")==0) { c.base=before; c.hasBase=true; }
		if(strcmp(event,"PRE")==0) c.turns=after;
		if(strcmp(event,"POST")==0 || strcmp(event,"POST_FALLBACK")==0){
			c.finalWeight=before; c.hasScore=true;
			if (!strcmp(event,"POST_FALLBACK")) { c.sanity=-1; c.reason="RAW_FALLBACK"; }
			if (decisionType=="ChooseHurry") c.sanity=before;
			c.turns=after;
			c.pool=(int)postPool.size();
			postPool.push_back(Key(k,obj));
		}  // The shared logger cannot bind NULL. Non-arithmetic events use zero placeholders;
		// their operation and reason carry the semantics, not those numeric columns.
		if(strcmp(event,"SELECTED")==0 || strcmp(event,"HURRY_SELECTED")==0){
			c.selected=1;
			winnerKindName=KindName(k);
			winnerObjectType=Symbol(k,obj);
		}  if(strstr(event,"CONTINUED") || strstr(event,"OVERRIDE"))selectionMode=event;
		if(strstr(event,"OVERRIDE") || strstr(event,"CONTINUED"))Term(event,"competition","ASSIGN",0,0,0,0,event,"");
	}
	void Weighted(const char* event,int a,int b,int c){
		if(!Enabled())return;
		selectionMode="WEIGHTED";
		if(strcmp(event,"POOL")==0 && a>=0 && a<(int)postPool.size())candidates[postPool[a]].eligible=b>=c?1:0;
		if(strcmp(event,"MODE_NO_CUTOFF")==0 || strcmp(event,"MODE_UNIFORM")==0)for(size_t i=0;i<postPool.size();++i)candidates[postPool[i]].eligible=1;
		if(strcmp(event,"WEIGHTED_DRAW")==0 || strcmp(event,"UNIFORM_DRAW")==0) {
			CvString detail; detail.Format("%s; draw=%d; %s=%d",event,a,strcmp(event,"UNIFORM_DRAW")==0 ? "pool_size" : "total_weight",b);
			Term(strcmp(event,"UNIFORM_DRAW")==0 ? "UNIFORM_SELECTION_DRAW" : "WEIGHTED_SELECTION_DRAW","selection","DRAW",0,0,0,0,"APPLIED",detail.c_str());
		}
	}
}
