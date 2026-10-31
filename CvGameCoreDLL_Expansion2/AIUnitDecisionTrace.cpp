// Production-AI decision diagnostics; SQLite backend remains in SqliteLogger.cpp.

#include "CvGameCoreDLLPCH.h"
#include "AIUnitDecisionTrace.h"
#include "SqliteLogger.h"
#include "CustomMods.h"

#include <set>
#include <string.h>

// must be included after all other headers
#include "LintFree.h"

namespace AIUnitDecisionTrace {
	namespace {
		int nextDecision=0, decision=0, ordinal=0, player=-1, city=-1, kind=-1, object=-1;
		int nextFlavorState=0, flavorState=-1;
		int nextObservation=0;
		CvString decisionType, decisionTrigger, selectedAction;
		CvString currentOrderObjectType, winnerKindName, winnerObjectType;
		bool consumedFlavor=false, observedAction=false;
		bool conflictingFlavorStates=false;
		std::set<std::pair<int,int> > persistedStateFlavors;
		bool registered=false;
		bool inDecision=false;
		struct Candidate {
			int player,city,kind,object,base,sanity,finalWeight,turns,eligible,selected,pool; bool hasBase,hasTerm,hasScore;
			struct Part { int flavor,effective,coefficient,product; CvString status; Components components; Part(int f,int e,int c,int p,const char* st,const Components& parts):flavor(f),effective(e),coefficient(c),product(p),status(st),components(parts) {} };
			std::vector<Part> flavors;
			CvString reason, flavorStatus;
			Candidate():player(-1),city(-1),kind(-1),object(-1),base(0),sanity(0),finalWeight(0),turns(-1),eligible(-1),selected(0),pool(-1),hasBase(false),hasTerm(false),hasScore(false),flavorStatus("NOT_APPLICABLE"){
			}
		};
		typedef std::pair<int,int> Key;
		std::map<Key,Candidate> candidates;
		CvString selectionMode;
		SqliteLogger::BatchWriter* termBatch=NULL;
		std::vector<Key> postPool;
		const char* termNames[]={
			"","FLAVOR_CACHE","CACHED_WEIGHT","ROLE_RECOMMENDATION","RECOMMEND_DURATION","BASE_COMPRESSION","PURCHASE_AND_CITY","CITY_MATURITY","MAINTENANCE","PLACEMENT","COUNTS","FORCE_LIMIT","MINOR_FORCE","LANDMASS","RESOURCE_REQUIRED","RESOURCE_SURPLUS","SPECIAL_ROLE","CITY_SUPPORT","SUICIDE_NUKE","PURCHASE_SURPLUS","EXPLORATION_CARRIER","NAVAL_DEFICIT","LAND_FORCE_DEFICIT","AIR_BALANCE","ANTI_AIR","CONQUEST","DIPLOMAT","SPACESHIP","RELIGION","TRADE","TRAIT","SETTLER","ARCHAEOLOGIST","WORKBOAT","WORKER","WAR_PRESSURE","BEST_CITY","PROMOTION","OPERATION_ROLE","DOMAIN_COUNTS","CONTINUATION","COMPLETION_FREE","ROLE_BALANCE","FINAL"
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
			GET_SQLITE_LOGGER().RegisterTable(name,schema);
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
			const char* d[]={"I:ObservationID","I:DecisionID","T:Player","T:City","I:FlavorStateID","T:DecisionType","T:DecisionTrigger","T:SelectionMode","T:WinnerKindName","T:WinnerObjectType","T:CurrentOrderObjectType","T:ProposedAction"};
			Register("AIProductionDecision",d,12);
			const char* c[]={"I:DecisionID","T:Player","T:City","T:DecisionType","T:SelectionMode","T:BuildableKindName","T:ObjectType","I:InitialWeight","I:SanityWeight","I:FinalWeight","I:EstimatedTurnsLeft","I:PoolIndex","I:Selected","T:Reason","T:InitialWeightBreakdownStatus"};
			Register("AIProductionCandidate",c,15);
			const char* t[]={"I:DecisionID","I:Sequence","T:Player","T:City","T:BuildableKindName","T:ObjectType","I:BeforeValue","I:Operand","I:AfterValue","T:Step","T:Target","T:VariableName","T:Operation","T:Reason"};
			Register("AIProductionTerm",t,14);
			const char* f[]={"I:DecisionID","T:Player","T:City","T:ObjectType","T:FlavorType","I:EffectiveCityFlavor","I:UnitFlavorWeight","I:Contribution","T:BuildableKindName","T:EffectiveCityFlavorBreakdownStatus"};
			Register("AIUnitFlavorContribution",f,10);
			const char* fc[]={"I:FlavorStateID","T:Player","T:City","T:FlavorType","T:ComponentSource","I:ComponentValue"};
			Register("AICityFlavorComponent",fc,6);
			const char* e[]={"I:ObservationID","I:DecisionID","T:Player","T:City","T:EventType","T:BuildableKindName","T:ObjectType","T:Currency","T:Result"};
			Register("AIProductionEvent",e,9);
			registered=true;
		}
		void Term(const char* id,const char* target,const char* op,int before,int operand,int after,int /*site*/,const char* event,const char* reason) {
			if(!Enabled())return;
			Ensure();
			candidates[Key(kind,object)].hasTerm=true;
			const char* variable="";
			const char* semanticTarget=target;
			if (!strcmp(target,"iBonus")) { variable=target; semanticTarget="SANITY_BONUS"; }
			else if (!strcmp(target,"iFlavorWeight")) {
				variable=target;
				semanticTarget=!strcmp(id,"t005") ? "SANITY_BASE" : "SANITY_SCORE";
			}
			else if (!strcmp(target,"sanity")) semanticTarget="SANITY_SCORE";
			else if (!strcmp(target,"recommendation")) semanticTarget="RECOMMENDATION_WEIGHT";
			else if (!strcmp(target,"competition")) semanticTarget="COMPETITION_WEIGHT";
			else if (!strcmp(target,"selection")) semanticTarget="SELECTION";
			if (!strcmp(op,"DURATION") && !strcmp(target,"competition")) variable="iTurns";
			const char* step=Name(id);
			if (!strcmp(op,"CUTOFF")) step="SELECTION_CUTOFF";
			else if (!strcmp(op,"DRAW")) step=id;
			else if (!strcmp(op,"SELECTED")) step="CANDIDATE_SELECTED";
			else if (!strcmp(op,"RECOMMENDED")) step="ROLE_RECOMMENDED";
			else if (!id[0] && !strcmp(op,"DURATION")) step="DURATION_REWEIGHTING";
			else if (!id[0]) step=event;
			termBatch->BeginLogRow().bind(decision).bind(++ordinal).bind(PlayerName(player).c_str()).bind(CityName(player,city).c_str()).bind(KindName(kind)).bind(Symbol(kind,object).c_str()).bind(before).bind(operand).bind(after).bind(step).bind(semanticTarget).bind(variable).bind(op).bind(reason).addRowToBatch();
		}
	}
	int CurrentDecision() { return Enabled() ? decision : 0; }
	void DecisionContext(const char* trigger) { if (Enabled()) decisionTrigger=trigger; }
	void DecisionAction(const char* action) { if (Enabled()) selectedAction=action; }
	void Duration(int before,int turns,int after) {
		if (!Enabled()) return;
		Candidate& c=candidates[Key(kind,object)]; c.hasScore=true;
		if (c.sanity==0) c.sanity=before;
		c.finalWeight=after;
		if (before!=after) Term("","competition","DURATION",before,turns,after,0,"APPLIED","");
	}
	void ProductionEvent(const CvCity* c,const char* type,int order,int obj,int origin,const char* currency,const char* result) {
		if (!Configured() || !GET_SQLITE_LOGGER().IsEnabled() || !c || obj<0) return;
		Ensure();
		const int k=OrderKind(order);
		if (Enabled() && origin==decision && player==c->getOwner() && city==c->GetID()) observedAction=true;
		GET_SQLITE_LOGGER().BeginLogRow("AIProductionEvent").bind(++nextObservation).bind(origin).bind(PlayerName(c->getOwner()).c_str()).bind(c->getName().c_str()).bind(type).bind(KindName(k)).bind(Symbol(k,obj).c_str()).bind(currency).bind(result).execute();
	}
	Action::Action(const CvCity* c,int origin,int order,int obj,const char* type,const char* currency):m_city(c),m_decision(origin),m_kind(order),m_object(obj),m_type(type),m_currency(currency),m_success(false),m_enabled(false) {
		// Shared purchase dispatchers also call these APIs with NO_UNIT/NO_BUILDING.
		m_enabled=Configured() && GET_SQLITE_LOGGER().IsEnabled() && obj>=0;
	}
	void Action::Success(const char* type) { m_success=true; if (type) m_type=type; }
	Action::~Action() {
		if (m_enabled) ProductionEvent(m_city,m_type,m_kind,m_object,m_decision,m_currency,m_success ? "APPLIED" : "FAILED");
	}
	bool Configured(){
		return MOD_AI_UNIT_DECISION_LOGGING && MOD_SQLITE_LOGGING;
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
	Scope::Scope(const char* caller,int p,int c):m_root(false),m_active(false),m_kind(kind),m_object(object),m_player(player),m_city(city),m_previousDecision(decision),m_previousInDecision(inDecision) {
		if(!Configured())return;

		if(!GET_SQLITE_LOGGER().IsEnabled())return;

		if(!inDecision && termBatch==NULL && (strcmp(caller,"ChooseProduction")==0 || strcmp(caller,"ChooseHurry")==0)) {
			m_root=true;
			inDecision=true;

			Ensure();

			termBatch=new SqliteLogger::BatchWriter(GET_SQLITE_LOGGER().BeginLogBatch("AIProductionTerm"));

			decision=++nextDecision;
			decisionType=caller;
			decisionTrigger="UNKNOWN";
			selectedAction="NONE";
			currentOrderObjectType="UNKNOWN";
			winnerKindName=winnerObjectType="UNKNOWN";
			consumedFlavor=observedAction=false;
			const CvCity* traceCity=GET_PLAYER((PlayerTypes)p).getCity(c);
			const OrderData* traceHead=traceCity ? traceCity->headOrderQueueNode() : NULL;
			if (traceHead) currentOrderObjectType=Symbol(OrderKind(traceHead->eOrderType),traceHead->iData1);

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
			// A process can evaluate a free unit using a fixed sanity input, not its own process identity.
			if (!strcmp(caller,"CheckUnitBuildSanity") && kind==CITY_BUILDABLE_PROCESS) kind=PROCESS_UNIT_PROXY;
		}
	}
	Scope::~Scope(){
		if(!m_active)return;
		if(m_root){
			// Discard read-only touches before testing scope content, regardless of mode or state ID.
			for (std::map<Key,Candidate>::iterator it=candidates.begin(); it!=candidates.end();) {
				const Candidate& c=it->second;
				if (!c.hasScore && !c.hasTerm && c.reason.empty() && c.pool<0 && !c.selected) candidates.erase(it++);
				else ++it;
			}

			termBatch->flush();

			delete termBatch;
			termBatch=NULL;
			if (!candidates.empty() || ordinal>0 || consumedFlavor || observedAction || selectedAction!="NONE") {
				SqliteLogger::BatchWriter kCandidateBatch = GET_SQLITE_LOGGER().BeginLogBatch("AIProductionCandidate");
				SqliteLogger::BatchWriter kFlavorBatch = GET_SQLITE_LOGGER().BeginLogBatch("AIUnitFlavorContribution");
				SqliteLogger::BatchWriter kComponentBatch = GET_SQLITE_LOGGER().BeginLogBatch("AICityFlavorComponent");
				for(std::map<Key,Candidate>::const_iterator it=candidates.begin();it!=candidates.end();++it){
					const Candidate& c=it->second;
					CvString symbol=Symbol(c.kind,c.object);
					CvString playerName=PlayerName(c.player), cityName=CityName(c.player,c.city);
					kCandidateBatch.BeginLogRow().bind(decision).bind(playerName.c_str()).bind(cityName.c_str()).bind(decisionType.c_str()).bind(selectionMode.c_str()).bind(KindName(c.kind)).bind(symbol.c_str()).bind(c.base).bind(c.sanity).bind(c.finalWeight).bind(c.turns).bind(c.eligible==1 ? c.pool : -1).bind(c.selected).bind(c.reason.c_str()).bind(c.flavorStatus.c_str()).addRowToBatch();
					for(size_t i=0;i<c.flavors.size();++i) {
						const Candidate::Part& f=c.flavors[i];
						const char* flavorName=GC.getFlavorTypes((FlavorTypes)f.flavor).c_str();
						if (!conflictingFlavorStates && flavorState>=0 && f.status!="UNAVAILABLE" && persistedStateFlavors.insert(std::make_pair(flavorState,f.flavor)).second) {
							for (size_t j=0; j<f.components.size(); ++j) {
								const Component& part=f.components[j];
								if (part.value) kComponentBatch.BeginLogRow().bind(flavorState).bind(playerName.c_str()).bind(cityName.c_str()).bind(flavorName).bind(part.source.c_str()).bind(part.value).addRowToBatch();
							}
						}
						kFlavorBatch.BeginLogRow().bind(decision).bind(playerName.c_str()).bind(cityName.c_str()).bind(symbol.c_str()).bind(flavorName).bind(f.effective).bind(f.coefficient).bind(f.product).bind(KindName(c.kind)).bind(conflictingFlavorStates ? "UNAVAILABLE" : f.status.c_str()).addRowToBatch();
					}
				}

				kCandidateBatch.flush();
				kFlavorBatch.flush();
				kComponentBatch.flush();
				// Allocate only as the finalized summary is emitted. Events retain native emission order.
				GET_SQLITE_LOGGER().BeginLogRow("AIProductionDecision").bind(++nextObservation).bind(decision).bind(PlayerName(player).c_str()).bind(CityName(player,city).c_str()).bind(conflictingFlavorStates ? -1 : flavorState).bind(m_caller).bind(decisionTrigger.c_str()).bind(selectionMode.c_str()).bind(winnerKindName.c_str()).bind(winnerObjectType.c_str()).bind(currentOrderObjectType.c_str()).bind(selectedAction.c_str()).execute();
			}
		}decision=m_previousDecision;
		inDecision=m_previousInDecision;
		kind=m_kind;
		object=m_object;
		player=m_player;
		city=m_city;
	}
	void Arithmetic(const char* id,int,int,int unit,int site,const char* target,const char* op,int before,int operand,int after){
		if(!Enabled())return;
		object=unit;
		if(strcmp(target,"iBonus") && strcmp(target,"iFlavorWeight"))return;
		const bool root=strcmp(id,"t005")==0 && strcmp(target,"iFlavorWeight")==0;
		if (!root && before==after) return;
		const char* operation = !strcmp(op,"+=")?"ADD":!strcmp(op,"-=")?"SUBTRACT":!strcmp(op,"*=")?"MULTIPLY":!strcmp(op,"/=")?"DIVIDE":"ASSIGN";
		const char* strategy="";
		if (!strcmp(id,"t042") && site>=124 && site<=132) {
			const char* names[]={"MILITARYAISTRATEGY_NEED_ARCHER","MILITARYAISTRATEGY_NEED_SIEGE","MILITARYAISTRATEGY_NEED_SKIRMISHER","MILITARYAISTRATEGY_NEED_MOBILE","MILITARYAISTRATEGY_NEED_NAVAL_MELEE","MILITARYAISTRATEGY_NEED_NAVAL_RANGED","MILITARYAISTRATEGY_NEED_SUBMARINE","MILITARYAISTRATEGY_NEED_BOMBER","MILITARYAISTRATEGY_NEED_FIGHTER"};
			strategy=names[site-124];
		}
		Term(id,target,root?"ROOT":operation,before,root?10:operand,after,site,"APPLIED",strategy);
	}
	int NewFlavorState() { return ++nextFlavorState; }
	void ConsumeFlavorState(int state) {
		if (!Enabled() || state<0) return;
		consumedFlavor=true;
		if (flavorState<0 && !conflictingFlavorStates) flavorState=state;
		else if (flavorState!=state) { flavorState=-1; conflictingFlavorStates=true; }
	}
	void FlavorStatus(bool complete) {
		if (!Enabled() || object<0) return;
		Candidate& c=candidates[Key(kind,object)]; c.flavors.clear(); c.flavorStatus=complete?"COMPLETE":"UNAVAILABLE";
	}
	void Flavor(int unit,int flavor,int effective,int coefficient,int product,const Components& parts,bool available) {
		if (!Enabled() || unit!=object || !product) return;
		int sum=0; for(size_t i=0;i<parts.size();++i) sum+=parts[i].value;
		const char* status=!available ? "UNAVAILABLE" : sum==effective ? "COMPLETE" : "INCOMPLETE";
		candidates[Key(kind,object)].flavors.push_back(Candidate::Part(flavor,effective,coefficient,product,status,parts));
	}
	void Event(const char* event,const char* id,int,int,int k,int obj,int before,int after,const char* field){
		if(!Enabled())return;
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
		if(strcmp(event,"NONFLAVOR_BASE")==0) { c.base=before; c.hasBase=true; c.flavors.clear(); c.flavorStatus="NOT_APPLICABLE"; }
		if(strcmp(event,"INPUT")==0 && strcmp(id,"t005")==0){c.hasScore=true; if(!c.hasBase) { c.base=before; c.hasBase=true; }}
		c.player=player;
		c.city=city;
		c.kind=k;
		c.object=obj;
		if((strcmp(event,"PRE")==0 && !c.hasBase) || strcmp(event,"BASE_WEIGHT")==0) { c.base=before; c.hasBase=true; }
		if(strcmp(event,"PRE")==0) { c.hasScore=true; c.turns=after; }
		if(strcmp(event,"POST")==0 || strcmp(event,"POST_FALLBACK")==0){
			c.finalWeight=before;
			c.turns=after;
			c.pool=(int)postPool.size();
			postPool.push_back(Key(k,obj));
		}  // The shared logger cannot bind NULL. Non-arithmetic events use zero placeholders;
		// their operation and reason carry the semantics, not those numeric columns.
		if(strcmp(event,"SELECTED")==0 || strcmp(event,"HURRY_SELECTED")==0){
			c.selected=1;
			winnerKindName=KindName(k);
			winnerObjectType=Symbol(k,obj);
			Term(id,"selection","SELECTED",0,0,0,0,"SELECTED",strcmp(event,"HURRY_SELECTED")==0 ? (after ? "FAITH" : "GOLD") : "");
		}  if(strstr(event,"CONTINUED") || strstr(event,"OVERRIDE"))selectionMode=event;
		if(strcmp(event,"RETURN")==0){
			c.sanity=after;
			if(after<=0)c.reason=field;
			if(after<=0) Term(id,"sanity","REJECT",before,0,after,0,"REJECTED",field);
			return;
		}  if(strcmp(event,"RECOMMEND_DURATION")==0)c.hasScore=true;
		if(strcmp(event,"RECOMMEND_DURATION")==0 && before!=after)Term("t004","recommendation","DURATION",before,0,after,0,"APPLIED","");
		if(strcmp(event,"ROLE_RECOMMENDED")==0)Term("t004","recommendation","RECOMMENDED",0,0,0,0,"RECOMMENDED","");
		if(strstr(event,"OVERRIDE") || strstr(event,"CONTINUED"))Term(event,"competition","ASSIGN",0,0,0,0,event,"");
	}
	void Weighted(const char* event,int a,int b,int c){
		if(!Enabled())return;
		selectionMode="WEIGHTED";
		if(strcmp(event,"POOL")==0 && a>=0 && a<(int)postPool.size())candidates[postPool[a]].eligible=b>=c?1:0;
		if(strcmp(event,"MODE_NO_CUTOFF")==0 || strcmp(event,"MODE_UNIFORM")==0)for(size_t i=0;i<postPool.size();++i)candidates[postPool[i]].eligible=1;
		if(strcmp(event,"CUTOFF")==0) Term("","selection","CUTOFF",b,a,c,0,"APPLIED","highest_weight;percent;cutoff_weight");
		else if(strcmp(event,"WEIGHTED_DRAW")==0 || strcmp(event,"UNIFORM_DRAW")==0) {
			CvString detail; detail.Format("%s; draw=%d; %s=%d",event,a,strcmp(event,"UNIFORM_DRAW")==0 ? "pool_size" : "total_weight",b);
			Term(strcmp(event,"UNIFORM_DRAW")==0 ? "UNIFORM_SELECTION_DRAW" : "WEIGHTED_SELECTION_DRAW","selection","DRAW",0,0,0,0,"APPLIED",detail.c_str());
		}
	}
}
