/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#include "CvGameCoreDLLPCH.h"
#include "CvGameCoreDLLUtil.h"
#include "CvUnitProductionAI.h"
#include "CvGameCoreUtils.h"
#include "CvInfosSerializationHelper.h"
#include "CvMilitaryAI.h"
#include "CvEconomicAI.h"
#include "CvDiplomacyAI.h"
#include "CvGrandStrategyAI.h"
#include "AIUnitDecisionTrace.h"
// include this after all other headers
#include "LintFree.h"


/// Constructor
CvUnitProductionAI::CvUnitProductionAI(CvCity* pCity,  CvUnitXMLEntries* pUnits):
	m_pCity(pCity),
	m_pUnits(pUnits),
	m_traceFlavorsComplete(false),
	m_traceFlavorState(-1),
	m_tracePreviousState(-1)
{
}

/// Destructor
CvUnitProductionAI::~CvUnitProductionAI(void)
{
}

/// Clear out AI local variables
void CvUnitProductionAI::Reset()
{
	ASSERT(m_pUnits != NULL, "Unit Production AI init failure: unit entries are NULL");

	m_traceFlavors.clear();
	m_traceComponents.clear();
	m_traceComponentsAvailable.clear();
	m_traceFlavorValues.clear();
	m_traceFlavorState = -1;
	m_traceFlavorsComplete = AIUnitDecisionTrace::Configured();
	if (m_traceFlavorsComplete && m_pUnits) m_traceFlavors.resize(m_pUnits->GetNumUnits());
	m_UnitAIWeights.clear();

	// Loop through reading each one and add an entry with 0 weight to our vector
	if(m_pUnits)
	{
		for(int i = 0; i < m_pUnits->GetNumUnits(); i++)
		{
			m_UnitAIWeights.push_back(i, 0);
		}
	}
}

template<typename UnitProductionAI, typename Visitor>
void CvUnitProductionAI::Serialize(UnitProductionAI& unitProductionAI, Visitor& visitor)
{
	visitor(unitProductionAI.m_UnitAIWeights);
}

/// Serialization read
void CvUnitProductionAI::Read(FDataStream& kStream)
{
	CvStreamLoadVisitor serialVisitor(kStream);
	Serialize(*this, serialVisitor);
	m_traceFlavors.clear();
	m_traceComponents.clear();
	m_traceComponentsAvailable.clear();
	m_traceFlavorValues.clear();
	m_traceFlavorsComplete = false;
	m_traceFlavorState = -1;
	m_tracePreviousState = -1;
	m_tracePreviousComponents.clear();
	m_tracePreviousAvailable.clear();
	m_tracePreviousValues.clear();
}

/// Serialization write
void CvUnitProductionAI::Write(FDataStream& kStream) const
{
	CvStreamSaveVisitor serialVisitor(kStream);
	Serialize(*this, serialVisitor);
}

FDataStream& operator>>(FDataStream& loadFrom, CvUnitProductionAI& writeTo)
{
	writeTo.Read(loadFrom);
	return loadFrom;
}
FDataStream& operator<<(FDataStream& saveTo, const CvUnitProductionAI& readFrom)
{
	readFrom.Write(saveTo);
	return saveTo;
}

void CvUnitProductionAI::TraceFlavorComponents(int flavor, int effective, const AIUnitDecisionTrace::Components& parts, bool available)
{
	if (!m_traceFlavorsComplete || flavor < 0) return;
	if (m_traceComponents.size() <= (size_t)flavor) {
		m_traceComponents.resize(flavor + 1);
		m_traceComponentsAvailable.resize(flavor + 1, false);
		m_traceFlavorValues.resize(flavor + 1, 0);
	}
	m_traceFlavorValues[flavor] = effective;
	m_traceComponents[flavor] = parts;
	m_traceComponentsAvailable[flavor] = available;
}

void CvUnitProductionAI::TraceFlavorUpdateComplete()
{
	if (!m_traceFlavorsComplete) return;
	bool same = m_tracePreviousState >= 0 && m_traceFlavorValues == m_tracePreviousValues &&
		m_traceComponentsAvailable == m_tracePreviousAvailable && m_traceComponents.size() == m_tracePreviousComponents.size();
	for (size_t f=0; same && f<m_traceComponents.size(); ++f) {
		const AIUnitDecisionTrace::Components& a=m_traceComponents[f];
		const AIUnitDecisionTrace::Components& b=m_tracePreviousComponents[f];
		if (a.size()!=b.size()) { same=false; break; }
		for (size_t i=0; same && i<a.size(); ++i) {
			bool found=false;
			for (size_t j=0; j<b.size(); ++j)
				if (a[i].source==b[j].source && a[i].value==b[j].value) { found=true; break; }
			if (!found) same=false;
		}
	}
	m_traceFlavorState = same ? m_tracePreviousState : AIUnitDecisionTrace::NewFlavorState();
	m_tracePreviousState = m_traceFlavorState;
	m_tracePreviousValues = m_traceFlavorValues;
	m_tracePreviousComponents = m_traceComponents;
	m_tracePreviousAvailable = m_traceComponentsAvailable;
}

/// Establish weights for one flavor; can be called multiple times to layer strategies
void CvUnitProductionAI::AddFlavorWeights(FlavorTypes eFlavor, int iWeight)
{
	if (iWeight==0)
		return;

	// Loop through all units
	for(int iUnit = 0; iUnit < m_pUnits->GetNumUnits(); iUnit++)
	{
		CvUnitEntry* entry = m_pUnits->GetEntry(iUnit);
		if(entry)
		{
			// Set its weight by looking at unit's weight for this flavor and using iWeight multiplier passed in
			const int coefficient = entry->GetFlavorValue(eFlavor);
			m_UnitAIWeights.IncreaseWeight(iUnit, coefficient * iWeight);
			if (m_traceFlavorsComplete && coefficient) m_traceFlavors[iUnit].push_back(FlavorPart(eFlavor, iWeight, coefficient));
		}
	}
}

/// Retrieve sum of weights on one item
int CvUnitProductionAI::GetWeight(UnitTypes eUnit)
{
	//dirty hack to allow barbarian cities to build units ... barbarians don't use flavors, so their weights are all zero
	if (m_pCity->getOwner() == BARBARIAN_PLAYER)
		return 1;

	const int weight = m_UnitAIWeights.GetWeight(eUnit);
	if (AIUnitDecisionTrace::Enabled()) TraceWeight(eUnit, weight);
	return weight;
}

void CvUnitProductionAI::TraceConsumeFlavorState()
{
	AIUnitDecisionTrace::ConsumeFlavorState(m_traceFlavorState);
}

void CvUnitProductionAI::TraceWeight(UnitTypes eUnit, int weight)
{
	if (!AIUnitDecisionTrace::Enabled()) return;
	TraceConsumeFlavorState();
	AIUnitDecisionTrace::Event("CACHED_WEIGHT", "t002", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, weight, 0);
	bool complete = m_traceFlavorsComplete && eUnit >= 0 && eUnit < (int)m_traceFlavors.size();
	int sum = 0;
	if (complete) for (size_t i=0; i<m_traceFlavors[eUnit].size(); ++i) sum += m_traceFlavors[eUnit][i].effective * m_traceFlavors[eUnit][i].coefficient;
	complete = complete && sum == weight;
	AIUnitDecisionTrace::FlavorStatus(complete);
	if (complete) for (size_t i=0; i<m_traceFlavors[eUnit].size(); ++i) {
		const FlavorPart& part = m_traceFlavors[eUnit][i];
		const bool available = part.flavor < (int)m_traceComponentsAvailable.size() && m_traceComponentsAvailable[part.flavor];
		AIUnitDecisionTrace::Flavor(eUnit, part.flavor, part.effective, part.coefficient, part.effective * part.coefficient, available ? m_traceComponents[part.flavor] : AIUnitDecisionTrace::Components(), available);
	}
}

/// Recommend highest-weighted unit
UnitTypes CvUnitProductionAI::RecommendUnit(UnitAITypes eUnitAIType, bool bAllowStrategicResource)
{
	AIUnitDecisionTrace::Scope traceScope("RecommendUnit", m_pCity->getOwner(), m_pCity->GetID());
	if(eUnitAIType <= NO_UNITAI)
	{
		return NO_UNIT;
	}

	// Reset list of all the possible units
	m_Buildables.clear();

	// Loop through adding the available units
	for(int iUnitLoop = 0; iUnitLoop < GC.GetGameUnits()->GetNumUnits(); iUnitLoop++)
	{
		const UnitTypes eUnit = static_cast<UnitTypes>(iUnitLoop);
		AIUnitDecisionTrace::SetCandidate(CITY_BUILDABLE_UNIT, eUnit);
		CvUnitEntry* pkUnitInfo = GC.getUnitInfo(eUnit);
		if(pkUnitInfo)
		{
			// Make sure it matches the requested unit AI type
			if (!pkUnitInfo->GetUnitAIType(eUnitAIType))
				continue;

			if (!bAllowStrategicResource && pkUnitInfo->GetResourceType() != NO_RESOURCE)
				continue;

			// Make sure this unit can be built now
			if(m_pCity->canTrain(eUnit, (m_pCity->isProductionUnit() && eUnit == m_pCity->getProductionUnit())))
			{
				// Update weight based on turns to construct
				int iTurnsLeft = m_pCity->getProductionTurnsLeft(eUnit, 0);
				int iTempWeight = m_UnitAIWeights.GetWeight(eUnit);
				if (AIUnitDecisionTrace::Enabled()) TraceWeight(eUnit, iTempWeight);
				if (iTempWeight == 0)
				{
					const int iNumFlavors = GC.getNumFlavorTypes();
					for (int iFlavorLoop = 0; iFlavorLoop < iNumFlavors; iFlavorLoop++)
					{
						iTempWeight += pkUnitInfo->GetFlavorValue(iFlavorLoop);
					}
					if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("NONFLAVOR_BASE", "t004", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iTempWeight, 0);
				}

				AIUnitDecisionTrace::SetObject(eUnit);
				int iWeight = CityStrategyAIHelpers::ReweightByTurnsLeft(iTempWeight, iTurnsLeft);
				if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RECOMMEND_DURATION", "t004", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iTempWeight, iWeight);
				if(iWeight > 0)
					m_Buildables.push_back(iUnitLoop, iWeight);
			}
		}

	}

	// Sort items and grab the first one
	if(m_Buildables.size() > 0)
	{
		m_Buildables.StableSortItems();
		LogPossibleBuilds(eUnitAIType);
		if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("ROLE_RECOMMENDED", "t004", m_pCity->getOwner(), m_pCity->GetID(), -1, m_Buildables.GetElement(0), eUnitAIType, m_Buildables.GetWeight(0));
		return (UnitTypes)m_Buildables.GetElement(0);
	}

	// Unless we didn't find any
	else
	{
		return NO_UNIT;
	}
}

int CvUnitProductionAI::CheckUnitBuildSanity(UnitTypes eUnit, bool bForOperation, int iFlavorWeight, bool bForPurchase, bool bFree)
{
	AIUnitDecisionTrace::Scope traceScope("CheckUnitBuildSanity", m_pCity->getOwner(), m_pCity->GetID());
	AIUnitDecisionTrace::SetObject(eUnit);
	if (AIUnitDecisionTrace::Enabled())
	{
		AIUnitDecisionTrace::Event("INPUT", "t005", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iFlavorWeight, (int)bForOperation, "FlavorWeight;ForOperation");
		AIUnitDecisionTrace::Event("INPUT", "t006", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, (int)bForPurchase, (int)bFree, "ForPurchase;Free");
	}
	//value to be added
	int iBonus = 0;
	int traceBonus = 0;
	if (MOD_AI_UNIT_DECISION_LOGGING) traceBonus = iBonus;

	if(iFlavorWeight < 1)
		{ const int traceResult = SR_IMPOSSIBLE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t005", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "NONPOSITIVE_INPUT_WEIGHT"); return traceResult; }

	int traceFlavorWeight = 0;
	if (MOD_AI_UNIT_DECISION_LOGGING) traceFlavorWeight = iFlavorWeight;

	//this seems to work well to bring the raw flavor weight into a sensible range [0 ... 200]
	iFlavorWeight = sqrti(10 * iFlavorWeight);
	if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ROOT("t005", 1, traceFlavorWeight, iFlavorWeight);

	CvUnitEntry* pkUnitEntry = GC.getUnitInfo(eUnit);
	if (!pkUnitEntry)
		{ const int traceResult = SR_IMPOSSIBLE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t005", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "UNIT_INFO_UNAVAILABLE"); return traceResult; }

	bool bCombat = pkUnitEntry->GetCombat() > 0 || pkUnitEntry->GetRangedCombat() > 0;
	bool bNeedsSupply = (pkUnitEntry->GetCombat() > 0 || pkUnitEntry->GetRangedCombat() > 0) && !pkUnitEntry->IsNoSupply();

	CvPlayerAI& kPlayer = GET_PLAYER(m_pCity->getOwner());
	if (!bFree && bForPurchase && !m_pCity->IsCanPurchase(/*bTestPurchaseCost*/ true, /*bTestTrainable*/ true, eUnit, NO_BUILDING, NO_PROJECT, YIELD_GOLD))
	{
		{ const int traceResult = SR_IMPOSSIBLE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t006", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "GOLD_PURCHASE_UNAVAILABLE"); return traceResult; }
	}

	if (!bFree && !bForPurchase && m_pCity->IsPuppet())
	{
		{ const int traceResult = SR_IMPOSSIBLE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t006", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "PUPPET_CANNOT_TRAIN"); return traceResult; }
	}

	bool bDesperate = m_pCity->isUnderSiege();
	if (!bFree && bDesperate && !bCombat)
	{
		{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t006", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "CITY_UNDER_SIEGE_NONCOMBAT_UNIT"); return traceResult; }
	}

	// Don't build units in underdeveloped cities
	if (m_pCity->GetCityBuildings()->GetNumBuildings() < 2 && !bDesperate && !bForPurchase)
	{
		{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t007", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "CITY_UNDERDEVELOPED"); return traceResult; }
	}

	// can we build an upgraded version of this unit in any of our cities?
	if (!kPlayer.GetUnitClassTrainingAllowedAnywhere().empty())
	{
		const set<UnitClassTypes>& sUnitClassTrainingAllowed = kPlayer.GetUnitClassTrainingAllowedAnywhere();
		for (set<UnitClassTypes>::const_iterator it = sUnitClassTrainingAllowed.begin(); it != sUnitClassTrainingAllowed.end(); ++it)
		{
			if (pkUnitEntry->GetUpgradeUnitClass(*it))
			{
				{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t007", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "UPGRADED_UNIT_AVAILABLE"); return traceResult; }
			}
		}
	}

	if (!bDesperate && !bFree)
	{
		if (bCombat && !pkUnitEntry->IsNoMaintenance() && !pkUnitEntry->IsTrade())
		{
			int iGoldSpentOnUnits = kPlayer.GetTreasury()->GetExpensePerTurnUnitMaintenance();
			int iAverageGoldPerUnit = iGoldSpentOnUnits / (max(1, kPlayer.getNumUnits()));
			int iTurnsLeft = kPlayer.getTurnsToBankruptcy(iAverageGoldPerUnit);

			if (iTurnsLeft < 9)
				{ const int traceResult = SR_MAINTENANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t008", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "UNIT_MAINTENANCE_BANKRUPTCY"); return traceResult; }
			else if (iTurnsLeft < 17)
			{
				iBonus -= iAverageGoldPerUnit*25;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t008", 2, traceBonus, iBonus);
			}
		}
	}

	//don't build land/sea units if there's no place to put them
	if (bCombat && m_pCity->HasGarrison() && pkUnitEntry->GetDomainType() != DOMAIN_AIR)
	{
		CvPlot* pCenter = m_pCity->plot();
		bool bHavePlot = false;
		for (int i = RING0_PLOTS; i < RING3_PLOTS && !bHavePlot; i++)
		{
			CvPlot* pNeighbor = iterateRingPlots(pCenter, i);
			if (pNeighbor && pNeighbor->isValidMovePlot(m_pCity->getOwner()) && pNeighbor->GetNumCombatUnits() == 0)
			{
				if (pNeighbor->isWater() && pkUnitEntry->GetDomainType() == DOMAIN_SEA)
					bHavePlot=true;

				if (!pNeighbor->isWater() && pkUnitEntry->GetDomainType() == DOMAIN_LAND)
					bHavePlot=true;
			}
		}

		if (!bHavePlot)
			{ const int traceResult = SR_IMPOSSIBLE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t009", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "NO_COMBAT_UNIT_PLACEMENT"); return traceResult; }
	}

	int iNumExplorers = kPlayer.GetNumUnitsWithUnitAI(UNITAI_EXPLORE, true, true);
	int iNumLandUnits = kPlayer.getNumMilitaryLandUnits() + kPlayer.GetNumUnitsInProduction(DOMAIN_LAND, true);
	int iNumSeaUnits = kPlayer.getNumMilitarySeaUnits() + kPlayer.GetNumUnitsInProduction(DOMAIN_SEA, true);

	UnitTypes eCurrentlyProducing = m_pCity->getProductionUnit();

	if (eCurrentlyProducing != NO_UNIT && !bForPurchase)
	{
		CvUnitEntry* pkCurrentlyProducing = GC.getUnitInfo(eCurrentlyProducing);
		if (pkCurrentlyProducing)
		{
			if (pkCurrentlyProducing->GetDomainType() == DOMAIN_LAND && pkCurrentlyProducing->GetDefaultUnitAIType() == UNITAI_EXPLORE)
				iNumExplorers--;
			else if (pkCurrentlyProducing->GetDomainType() == DOMAIN_LAND && (pkCurrentlyProducing->GetCombat() > 0 || pkCurrentlyProducing->GetRangedCombat() > 0))
				iNumLandUnits--;
			else if (pkCurrentlyProducing->GetDomainType() == DOMAIN_SEA && (pkCurrentlyProducing->GetCombat() > 0 || pkCurrentlyProducing->GetRangedCombat() > 0))
				iNumSeaUnits--;
		}
	}

	int iNumTotalUnits = iNumLandUnits + iNumSeaUnits;

	iNumLandUnits -= iNumExplorers;

	if (bNeedsSupply)
	{
		if (pkUnitEntry->GetDomainType() == DOMAIN_LAND && pkUnitEntry->GetDefaultUnitAIType() != UNITAI_EXPLORE && iNumLandUnits >= kPlayer.GetMilitaryAI()->GetRecommendLandArmySize())
			{ const int traceResult = SR_UNITSUPPLY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t011", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "RECOMMENDED_LAND_FORCE_REACHED"); return traceResult; }
		else if (pkUnitEntry->GetDomainType() == DOMAIN_SEA && iNumSeaUnits >= kPlayer.GetMilitaryAI()->GetRecommendNavySize())
			{ const int traceResult = SR_UNITSUPPLY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t011", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "RECOMMENDED_NAVY_REACHED"); return traceResult; }
		else if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_EXPLORE && iNumExplorers >= kPlayer.GetMilitaryAI()->GetRecommendedExplorers())
			{ const int traceResult = SR_UNITSUPPLY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t011", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "RECOMMENDED_EXPLORERS_REACHED"); return traceResult; }
	}

	//only war with majors count
	bool bAtWar = (kPlayer.GetMilitaryAI()->GetNumberCivsAtWarWith(false) > 0);
	if (!bFree && kPlayer.isMinorCiv())
	{
		if (bNeedsSupply)
		{
			if (iNumTotalUnits > max(4, ((kPlayer.GetCurrentEra() + 3) * max(1, kPlayer.getNumCities()))))
				{ const int traceResult = SR_UNITSUPPLY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t012", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MINOR_TOTAL_FORCE_LIMIT"); return traceResult; }

			if (pkUnitEntry->GetDomainType() == DOMAIN_SEA)
			{
				if (iNumSeaUnits <= 2)
				{
					iBonus += max(0, 300 - (iNumSeaUnits * 100));
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t012", 3, traceBonus, iBonus);
				}

				int iLimit = kPlayer.GetMilitaryAI()->GetRecommendNavySize();
				if (iNumSeaUnits < iLimit)
				{
					iBonus += (iLimit - iNumSeaUnits) * 100;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t012", 4, traceBonus, iBonus);
					if (kPlayer.GetMinorCivAI()->IsRecentlyBulliedByAnyMajor())
					{
						iBonus += 100;
						if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t012", 5, traceBonus, iBonus);
					}
				}
				else
					{ const int traceResult = SR_UNITSUPPLY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t012", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MINOR_RECOMMENDED_NAVY_REACHED"); return traceResult; }
			}
			else if (pkUnitEntry->GetDomainType() == DOMAIN_LAND)
			{
				if (iNumLandUnits <= 2)
				{
					iBonus += max(0, 800 - (iNumLandUnits * 100));
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t012", 6, traceBonus, iBonus);
				}

				int iLimit = kPlayer.GetMilitaryAI()->GetRecommendLandArmySize();
				if (iNumLandUnits < iLimit)
				{
					iBonus += (iLimit - iNumLandUnits) * 150;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t012", 7, traceBonus, iBonus);
					if (kPlayer.GetMinorCivAI()->IsRecentlyBulliedByAnyMajor())
					{
						iBonus += 250;
						if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t012", 8, traceBonus, iBonus);
					}
				}
				else
					{ const int traceResult = SR_UNITSUPPLY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t012", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MINOR_RECOMMENDED_LAND_FORCE_REACHED"); return traceResult; }
			}
		}
	}

	//Are we alone?
	DomainTypes eDomain = pkUnitEntry->GetDomainType();
	if (!bFree && bCombat)
	{
		CvLandmass* pLM = GC.getMap().getLandmassById(m_pCity->plot()->getLandmass());
		if(pLM != NULL && pLM->getNumTiles() <= 3)
		{
			if (eDomain == DOMAIN_LAND && m_pCity->HasGarrison() && !bForOperation)
			{
				CvUnit* pGarrison = m_pCity->GetGarrisonedUnit();
				if (pGarrison->getDomainType() == DOMAIN_LAND)
					{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t013", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "SMALL_LANDMASS_ALREADY_GARRISONED"); return traceResult; }
			}
		}
	}

	
	//////////////////
	//DOMAINS AND LOCATION CHECKS
	////////////////////////

	// sanity check for building ships on isolated small inland seas (not lakes)
	// if needed allow workboats...
	// shouldn't be needed any longer
	/*
	if (eDomain == DOMAIN_SEA && pkUnitEntry->GetCombat() > 0) 
	{
		int iWaterTiles = 0;
		int iNumUnitsofMine = 0;
		int iNumUnitsOther = 0;
		int iNumCitiesofMine = 0;
		int iNumCitiesOther = 0;

		//todo: alternatively check whether we can create a sea trade route to another player?
		std::vector<int> landmasses = m_pCity->plot()->getAllAdjacentLandmasses();
		for (std::vector<int>::iterator it=landmasses.begin(); it!=landmasses.end(); ++it)
		{
			CvLandmass* pkLandmass = GC.getMap().getLandmassById(*it);
			if (pkLandmass->isWater())
			{
				iWaterTiles += pkLandmass->getNumTiles();
				iNumUnitsofMine += pkLandmass->getUnitsPerPlayer(m_pCity->getOwner());
				iNumUnitsOther += pkLandmass->getNumUnits()-iNumUnitsofMine;
				iNumCitiesofMine += pkLandmass->getCitiesPerPlayer(m_pCity->getOwner());
				iNumCitiesOther += pkLandmass->getNumCities()-iNumCitiesofMine;
			}
		}

		int iFactor = GD_INT_GET(AI_CONFIG_MILITARY_TILES_PER_SHIP);
		//Are we mustering a naval attack here?
		bool bOperationalOverride = (bForOperation && kPlayer.IsMusterCityForOperation(m_pCity, true));
		bool bTooManyUnits = (iNumUnitsofMine * iFactor > iWaterTiles);
		bool bNoEnemies = (iNumUnitsOther < 1 && iNumCitiesOther < 1);

		if (!bFree && !bOperationalOverride && (bTooManyUnits || bNoEnemies))
		{
			return SR_USELESS;
		}
	}
	*/
	if(!kPlayer.isMinorCiv())
	{
		/////////
		//RESOURCE CHECKS
		////////////
		//Check for special unlimited units from specific resources (not a strategic check)
		if(pkUnitEntry->GetResourceType() != NO_RESOURCE)
		{
			if(kPlayer.getNumResourceAvailable((ResourceTypes)pkUnitEntry->GetResourceType(), false) > 0)
			{
				iBonus += 5 * kPlayer.getNumResourceAvailable((ResourceTypes)pkUnitEntry->GetResourceType(), false);
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t014", 9, traceBonus, iBonus);
			}
			else
			{
				{ const int traceResult = SR_IMPOSSIBLE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t014", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "REQUIRED_RESOURCE_UNAVAILABLE"); return traceResult; }
			}
		}
		//Check for specific resource usage by units.
		if(pkUnitEntry->GetSpaceshipProject() == NO_PROJECT)
		{
			if(kPlayer.GetDiplomacyAI()->IsGoingForSpaceshipVictory() || kPlayer.GetDiplomacyAI()->IsCloseToSpaceshipVictory())
			{
				//Aluminum Check
				static ResourceTypes eAluminumResource = (ResourceTypes)GC.getInfoTypeForString("RESOURCE_ALUMINUM", true);
				if(pkUnitEntry->GetResourceQuantityRequirement(eAluminumResource) > 0)
				{
					if(kPlayer.getNumResourceAvailable(eAluminumResource, false) <= (kPlayer.GetNumAluminumStillNeededForSpaceship() + kPlayer.GetNumAluminumStillNeededForCoreCities()))
					{
						{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t014", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "ALUMINUM_RESERVED_FOR_SPACESHIP_AND_CORE_CITIES"); return traceResult; }
					}
				}
			}

			ResourceTypes eNeededResource = (ResourceTypes)pkUnitEntry->GetResourceType();
			if (eNeededResource != NO_RESOURCE)
			{	
				//if we have a lot of the resource, we should spend it ... the units are usually good
				iBonus += max(40, (kPlayer.getNumResourceAvailable(eNeededResource, false) - pkUnitEntry->GetResourceQuantityRequirement(eNeededResource)) * 40);
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t015", 10, traceBonus, iBonus);
			}
		}

		///////////////
		//UNIT TYPE CHECKS
		//////////////////////
		//Virtually force this.
		if (m_pCity->IsUnitInvestment((UnitClassTypes)pkUnitEntry->GetUnitClassType()))
		{
			iBonus += 500;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t015", 11, traceBonus, iBonus);
		}

		if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_CITY_SPECIAL)
		{
			// Roughly one per 6 units seems sensible
			int iNum = kPlayer.GetNumUnitsWithUnitAI(UNITAI_CITY_SPECIAL, true);
			if (eCurrentlyProducing != NO_UNIT && !bForPurchase)
			{
				CvUnitEntry* pkCurrentlyProducing = GC.getUnitInfo(eCurrentlyProducing);
				if (pkCurrentlyProducing)
				{
					if (pkCurrentlyProducing->GetDefaultUnitAIType() == UNITAI_CITY_SPECIAL)
						iNum--;
				}
			}
			int iNumArmies = iNumLandUnits / 6;
			if (iNum < iNumArmies)
			{
				iBonus += 2000 * max(3 - iNum, 1);
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t016", 12, traceBonus, iBonus);

				// If we are planning a city attack, pretty much force this
				if (bForOperation)
				{
					iBonus += 10000;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t016", 13, traceBonus, iBonus);
				}
			}
		}

		//Sanity check for buildable support units.
		if (!bFree && pkUnitEntry->IsCityAttackSupport() && !bForOperation)
		{
			int iTotalAlready = kPlayer.GetNumUnitsWithUnitAI(pkUnitEntry->GetDefaultUnitAIType(), false);
			int iNum = kPlayer.GetNumUnitsWithUnitAI(UNITAI_CITY_BOMBARD, true);
			if (iNum < iTotalAlready)
			{
				{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t017", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "CITY_ATTACK_SUPPORT_EXCEEDS_BOMBARD_UNITS"); return traceResult; }
			}
			else
			{
				iBonus += 10 * (5 - iTotalAlready);
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t017", 14, traceBonus, iBonus);
			}
		}

		//Suicide units? Hmm...
		if(pkUnitEntry->IsSuicide())
		{
			//Nukes!!!
			if(pkUnitEntry->GetNukeDamageLevel() > 0)
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t018", 15, traceBonus, iBonus);
			}
			//Cruise Missiles? Only if we don't have any nukes lying around...
			else if(pkUnitEntry->GetRangedCombat() > 0 && kPlayer.getNumNukeUnits() > 0)
			{
				iBonus -= 50;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t018", 16, traceBonus, iBonus);
			}
			else
			{
				iBonus += 25;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t018", 17, traceBonus, iBonus);
			}
		}


		//Policy unlock or move-on-purchase? These are usually cheap and good, so get em!
		if(pkUnitEntry->GetPolicyType() != NO_POLICY || pkUnitEntry->CanMoveAfterPurchase())
		{
			if(bCombat && bAtWar)
			{
				int iGoldCost = m_pCity->GetPurchaseCost(eUnit);
				int iSurplus = ((kPlayer.GetTreasury()->GetGold() - iGoldCost) / 10);
				if(iSurplus > 0)
				{
					//Bonus based on difference in gold - the more money we have, the more we want this!
					iBonus += min(250, iSurplus);
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t019", 18, traceBonus, iBonus);
				}
			}
			else if(bCombat && !bAtWar)
			{
				int iGoldCost = m_pCity->GetPurchaseCost(eUnit);
				int iSurplus = ((kPlayer.GetTreasury()->GetGold() - iGoldCost) / 25);
				if(iSurplus > 0)
				{
					//Bonus based on difference in gold - the more money we have, the more we want this!
					iBonus += min(200, iSurplus);
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t019", 19, traceBonus, iBonus);
				}
			}
			else if(!bCombat)
			{
				int iGoldCost = m_pCity->GetPurchaseCost(eUnit);
				int iSurplus = ((kPlayer.GetTreasury()->GetGold() - iGoldCost) / 50);
				if(iSurplus > 0)
				{
					//Bonus based on difference in gold - the more money we have, the more we want this!
					iBonus += min(150, iSurplus);
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t019", 20, traceBonus, iBonus);
				}
			}
		}

		//Carriers? Only if we need them.
		if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_CARRIER_SEA)
		{
			if (kPlayer.isMinorCiv())
				{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t020", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MINOR_CIV_CARRIER"); return traceResult; }

			if (!bForOperation && !MilitaryAIHelpers::IsTestStrategy_NeedAirCarriers(&kPlayer))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t020", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "AIR_CARRIER_NOT_NEEDED"); return traceResult; }
		}

		//Need Explorers?
		if (eDomain == DOMAIN_LAND && pkUnitEntry->GetDefaultUnitAIType() == UNITAI_EXPLORE)
		{
			int iExplorersNeeded = kPlayer.GetMilitaryAI()->GetRecommendedExplorers();

			int iExploreBonus = iExplorersNeeded - iNumExplorers;
			if (iExploreBonus > 0)
			{
				iBonus += iExploreBonus * 500;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t020", 21, traceBonus, iBonus);
			}
		}

		//Need Sea Explorers?
		/*if (EconomicAIHelpers::IsPotentialNavalExplorer(pkUnitEntry->GetDefaultUnitAIType()))
		{
			int iExplorersNeeded = kPlayer.GetEconomicAI()->GetNavalExplorersNeeded(); //this will always return at least one!
			int iExplorersHave = kPlayer.GetNumUnitsWithUnitAI(UNITAI_EXPLORE_SEA, true);
			int iExplorersIdle = kPlayer.GetNumUnitsWithUnitAI(pkUnitEntry->GetDefaultUnitAIType(), true);

			if (iExplorersNeeded > iExplorersHave + iExplorersIdle)
				iBonus += 500;

		}*/
		//Naval Units Critically Needed?
		if (eDomain == DOMAIN_SEA)
		{
			if(bCombat)
			{
				// Are we an actual port city?
				if (!m_pCity->IsConnectedToOcean())
					{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t021", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "CITY_NOT_CONNECTED_TO_OCEAN"); return traceResult; }

				int iCurrent = iNumSeaUnits;
				int iDesired = kPlayer.GetMilitaryAI()->GetRecommendNavySize();
				int iValue = iDesired - iCurrent;

				// German UA encourages producing lots of units to gift to City-States
				if (iValue > 0 && kPlayer.GetPlayerTraits()->GetMinorInfluencePerGiftedUnit() > 0)
				{
					iBonus += 75;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t021", 22, traceBonus, iBonus);
				}

				iValue *= max(1, (int)kPlayer.GetCurrentEra());

				if (iValue > 0 && !kPlayer.isBarbarian())
				{
					//emphasize navy if there is nobody to attack over land
					if (MilitaryAIHelpers::IsTestStrategy_NeedNavalUnitsCritical(&kPlayer))
						iValue *= 5;
					else if (MilitaryAIHelpers::IsTestStrategy_NeedNavalUnits(&kPlayer))
						iValue *= 2;

					const CivsList& warPlayers = kPlayer.GetPlayersAtWarWith();
					vector<PlayerTypes> vUnfriendlyMajors = kPlayer.GetUnfriendlyMajors();
					int iNumPlayers = (int)warPlayers.size();
					int iWarValue = 0;
					for (size_t i=0; i<warPlayers.size(); i++)
					{
						PlayerTypes eLoopPlayer = warPlayers[i];

						// War type is only tracked for major civs
						if (GET_PLAYER(eLoopPlayer).isMinorCiv())
							continue;

						if (kPlayer.GetMilitaryAI()->GetWarType(eLoopPlayer) == WARTYPE_SEA)
						{
							if (std::find(vUnfriendlyMajors.begin(), vUnfriendlyMajors.end(), eLoopPlayer) != vUnfriendlyMajors.end())
								iWarValue += 4;
							else
								iWarValue += 2;
						}
						else
						{
							iWarValue += 1;
						}
					}

					iWarValue /= max(1, iNumPlayers);

					if (kPlayer.GetMilitaryAI()->GetWarType() == WARTYPE_SEA)
					{
						iWarValue += 10;
					}

					if (iWarValue > 0)
					{
						iValue *= iWarValue;
					}
				}
				iBonus += iValue * 30;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t021", 23, traceBonus, iBonus);
			}
		}
		//Land Units Critically Needed?
		else if (eDomain == DOMAIN_LAND)
		{
			if (bCombat)
			{
				int iCurrent = iNumLandUnits;
				int iDesired = kPlayer.GetMilitaryAI()->GetRecommendLandArmySize();
				int iValue = iDesired - iCurrent;

				// German UA encourages producing lots of units to gift to City-States
				if (iValue > 0 && kPlayer.GetPlayerTraits()->GetMinorInfluencePerGiftedUnit() > 0)
				{
					iBonus += 150;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t022", 24, traceBonus, iBonus);
				}

				iValue *= max(1, (int)kPlayer.GetCurrentEra());

				if (iValue > 0 && !kPlayer.isBarbarian())
				{
					const CivsList& warPlayers = kPlayer.GetPlayersAtWarWith();
					vector<PlayerTypes> vUnfriendlyMajors = kPlayer.GetUnfriendlyMajors();
					int iNumPlayers = (int)warPlayers.size();
					int iWarValue = 0;
					for (size_t i=0; i<warPlayers.size(); i++)
					{
						PlayerTypes eLoopPlayer = warPlayers[i];

						// War type is only tracked for major civs
						if (GET_PLAYER(eLoopPlayer).isMinorCiv())
							continue;

						if (kPlayer.GetMilitaryAI()->GetWarType(eLoopPlayer) == WARTYPE_LAND)
						{
							if (std::find(vUnfriendlyMajors.begin(), vUnfriendlyMajors.end(), eLoopPlayer) != vUnfriendlyMajors.end())
								iWarValue += 4;
							else
								iWarValue += 2;
						}
						else
						{
							iWarValue += 1;
						}
					}

					iWarValue /= max(1, iNumPlayers);

					if (kPlayer.GetMilitaryAI()->GetWarType() == WARTYPE_LAND)
					{
						iWarValue += 15;
					}

					if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_DEFENSE || (!MOD_AI_UNIT_PRODUCTION && pkUnitEntry->GetDefaultUnitAIType() == UNITAI_COUNTER) || pkUnitEntry->GetDefaultUnitAIType() == UNITAI_ATTACK)
					{
						CvUnit* pLoopUnit2 = NULL;
						for (int iUnitLoop = 0; iUnitLoop < m_pCity->plot()->getNumUnits(); iUnitLoop++)
						{
							pLoopUnit2 = m_pCity->plot()->getUnitByIndex(iUnitLoop);
							if (pLoopUnit2->isFound() && pLoopUnit2->getArmyID() == -1)
							{
								iWarValue += 2;
							}
							else if (pLoopUnit2->isFound() && pLoopUnit2->getArmyID() != -1)
							{
								iWarValue += 1;
							}
						}
					}

					if (iWarValue > 0)
					{
						iValue *= iWarValue;
					}
				}
				iBonus += iValue * 30;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t022", 25, traceBonus, iBonus);
			}
		}

		if (eDomain == DOMAIN_AIR)
		{
			//if we can build an airforce do so, independent of other players
			//just take care that it's approximately evenly split between attack and defense
			int ourBombers = kPlayer.GetNumUnitsWithUnitAI(UNITAI_ATTACK_AIR, true);
			int ourFighters = kPlayer.GetNumUnitsWithUnitAI(UNITAI_DEFENSE_AIR, true);
			int emptySlots = m_pCity->GetMaxAirUnits() - m_pCity->plot()->countNumAirUnits(kPlayer.getTeam(), true);

			switch (pkUnitEntry->GetDefaultUnitAIType())
			{
			case UNITAI_ATTACK_AIR:
			{
				if (ourBombers * 2 > ourFighters * 3)
					{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t023", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "BOMBERS_EXCEED_FIGHTER_BALANCE"); return traceResult; }

				iBonus += 2000 + emptySlots*100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t023", 26, traceBonus, iBonus);
				break;
			}
			case UNITAI_DEFENSE_AIR:
			{
				if (ourFighters * 2 > ourBombers * 3)
					{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t023", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "FIGHTERS_EXCEED_BOMBER_BALANCE"); return traceResult; }

				iBonus += 2000 + emptySlots*100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t023", 27, traceBonus, iBonus);
				break;
			}
			default: //missiles etc
				iBonus += 1000;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t023", 28, traceBonus, iBonus);
			}
		}

		//Air defense needed?
		if ((eDomain == DOMAIN_LAND && pkUnitEntry->GetAirInterceptRange() > 0) || pkUnitEntry->GetBaseLandAirDefense() > 20)
		{
			int iNeedAir = 0;
			int iNumAA = kPlayer.GetMilitaryAI()->GetNumAAUnits();
			int iOurAir = kPlayer.GetNumUnitsWithUnitAI(UNITAI_ATTACK_AIR, false) + kPlayer.GetNumUnitsWithUnitAI(UNITAI_DEFENSE_AIR, false);
			for (int iPlayerLoop = 0; iPlayerLoop < MAX_MAJOR_CIVS; iPlayerLoop++)
			{
				PlayerTypes eLoopPlayer = (PlayerTypes)iPlayerLoop;

				if (eLoopPlayer != kPlayer.GetID() && kPlayer.GetDiplomacyAI()->IsPlayerValid(eLoopPlayer) &&
					(kPlayer.GetProximityToPlayer(eLoopPlayer) == PLAYER_PROXIMITY_NEIGHBORS || GET_TEAM(kPlayer.getTeam()).isAtWar(GET_PLAYER(eLoopPlayer).getTeam())))
				{
					int iTheirAir = GET_PLAYER(eLoopPlayer).GetNumUnitsWithUnitAI(UNITAI_DEFENSE_AIR, false) + GET_PLAYER(eLoopPlayer).GetNumUnitsWithUnitAI(UNITAI_ATTACK_AIR, false);

					//they have no air units? Ignore!
					if (iTheirAir == 0)
						continue;

					//not enough AA?
					if (iNumAA < kPlayer.getNumCities())
						iNeedAir += kPlayer.getNumCities();

					//they're superior to us overall? Get on it!
					if (iTheirAir >= iOurAir)
						iNeedAir += kPlayer.getNumCities();
				}
			}

			iBonus += iNeedAir * 50;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t024", 29, traceBonus, iBonus);
		}
	
		/////////////
		//GRAND STRATEGY CHECKS
		//////////////////
		static EconomicAIStrategyTypes eStrategyConquest = (EconomicAIStrategyTypes) GC.getInfoTypeForString("ECONOMICAISTRATEGY_GS_CONQUEST");
		if(pkUnitEntry->GetCombat() > 0)
		{
			if(eStrategyConquest != NO_ECONOMICAISTRATEGY && kPlayer.GetEconomicAI()->IsUsingStrategy(eStrategyConquest))
			{
				iBonus += 50;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t025", 30, traceBonus, iBonus);
			}
		}

		int iProductionBonus = kPlayer.GetUnitClassProductionModifier((UnitClassTypes)pkUnitEntry->GetUnitClassType());
		if (iProductionBonus > 0)
		{
			iBonus += (iProductionBonus / 5);
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t025", 31, traceBonus, iBonus);
		}

		//Diplomatic Units!
		if (MOD_BALANCE_VP && pkUnitEntry->GetDefaultUnitAIType() == UNITAI_MESSENGER)
		{
			if (!kPlayer.HasMetValidMinorCiv())
				{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t026", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "NO_MET_VALID_MINOR_CIV"); return traceResult; }

			//There's a diplomat waiting here? Abort!
			if (m_pCity->plot()->getNumUnitsOfAIType(UNITAI_MESSENGER, m_pCity->getOwner()) > 0)
			{
				{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t026", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MESSENGER_ALREADY_IN_CITY"); return traceResult; }
			}

			//There's 2 sitting around? Abort!
			if (kPlayer.GetNumUnitsWithUnitAI(UNITAI_MESSENGER, true) > 3)
			{
				{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t026", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MESSENGER_COUNT_EXCEEDS_LIMIT"); return traceResult; }
			}

			int iInfluence = 0;
			//Promotion Bonus
			for (int iI = 0; iI < GC.getNumPromotionInfos(); iI++)
			{
				PromotionTypes ePromotion = (PromotionTypes)iI;
				if (pkUnitEntry->GetFreePromotions(iI))
					iInfluence += GC.getPromotionInfo(ePromotion)->GetDiploMissionInfluence();

				if (kPlayer.GetFreePromotionCount((PromotionTypes)iI))
					iInfluence += GC.getPromotionInfo(ePromotion)->GetDiploMissionInfluence();
			}

			//should not iterate all promotion types ... this way is faster
			vector<PromotionTypes> freePromotions = m_pCity->getFreePromotions();
			for (size_t iI = 0; iI < freePromotions.size(); iI++)
			{
				iInfluence += GC.getPromotionInfo(freePromotions[iI])->GetDiploMissionInfluence();
			}

			static EconomicAIStrategyTypes eNeedDiplomats = (EconomicAIStrategyTypes) GC.getInfoTypeForString("ECONOMICAISTRATEGY_NEED_DIPLOMATS");
			static EconomicAIStrategyTypes eNeedDiplomatsCrit = (EconomicAIStrategyTypes) GC.getInfoTypeForString("ECONOMICAISTRATEGY_NEED_DIPLOMATS_CRITICAL");
			if(kPlayer.GetEconomicAI()->IsUsingStrategy(eNeedDiplomats))
			{
				iInfluence *= 2;
			}
			else if(kPlayer.GetEconomicAI()->IsUsingStrategy(eNeedDiplomatsCrit))
			{
				iInfluence *= 3;
			}
			else
			{
				{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t026", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "DIPLOMAT_NEED_STRATEGIES_INACTIVE"); return traceResult; }
			}

			if (MOD_BALANCE_VP)
			{
				static ResourceTypes ePaper = (ResourceTypes)GC.getInfoTypeForString("RESOURCE_PAPER", true);
				if (ePaper != NO_RESOURCE && kPlayer.getResourceFromCSAlliances(ePaper) > 0)
				{
					iInfluence *= 2;
				}
			}

			iBonus += iInfluence;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t026", 38, traceBonus, iBonus);
		}

		if(pkUnitEntry->GetSpaceshipProject() != NO_PROJECT)
		{
			// For the AI, spaceship production is controlled in AI_doSpaceshipAndUtopiaProduction, overriding normal AI production selection
			if (!kPlayer.isHuman(ISHUMAN_AI_CITY_PRODUCTION) && !kPlayer.isMinorCiv())
			{
				{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t027", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "SPACESHIP_PRODUCTION_CONTROLLED_SEPARATELY"); return traceResult; }
			}

			static EconomicAIStrategyTypes eStrategySS = (EconomicAIStrategyTypes) GC.getInfoTypeForString("ECONOMICAISTRATEGY_GS_SPACESHIP");
			if (eStrategySS != NO_ECONOMICAISTRATEGY && kPlayer.GetEconomicAI()->IsUsingStrategy(eStrategySS))
			{
				iBonus += 10000;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t027", 39, traceBonus, iBonus);
			}
			else
			{
				iBonus += 1000;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t027", 40, traceBonus, iBonus);
			}
			if(kPlayer.GetDiplomacyAI()->IsCloseToSpaceshipVictory())
			{
				iBonus += 10000;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t027", 41, traceBonus, iBonus);
			}
			else
			{
				iBonus += 2500;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t027", 42, traceBonus, iBonus);
			}

			if(m_pCity->getSpaceProductionModifier() > 0)
			{
				iBonus += (m_pCity->getSpaceProductionModifier() * 100);
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t027", 43, traceBonus, iBonus);
			}
			//Don't zero out because of this penalty.
			if (iBonus <= 0)
			{
				iBonus = 1;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ASSIGN("t027", 44, traceBonus, iBonus);
			}
		}

		/////////
		//RELIGION CHECKS
		////////////
		if(pkUnitEntry->GetCombat() > 0)
		{
			int iReligiousBonus = 0;
			ReligionTypes eReligion = GET_PLAYER(m_pCity->getOwner()).GetReligions()->GetStateReligion();

			if (eReligion != NO_RELIGION)
			{
				const CvReligion* pReligion = GC.getGame().GetGameReligions()->GetReligion(eReligion, m_pCity->getOwner());
				if (pReligion)
				{
					CvBeliefXMLEntries* pkBeliefs = GC.GetGameBeliefs();
					for (size_t iI = 0; iI < pReligion->m_Beliefs.GetBeliefList().size(); iI++)
					{
						const BeliefTypes eBelief = (BeliefTypes) pReligion->m_Beliefs.GetBeliefList()[iI];
						CvBeliefEntry* pEntry = pkBeliefs->GetEntry(eBelief);
						if (pEntry && pReligion->m_Beliefs.HasBelief(eBelief) && pReligion->m_Beliefs.IsBeliefValid(eBelief, eReligion, m_pCity->getOwner()))
						{
							if (pEntry->GetFaithFromKills() > 0 && (pkUnitEntry->GetUnitAIType(UNITAI_ATTACK) || pkUnitEntry->GetUnitAIType(UNITAI_FAST_ATTACK)))
							{
								iReligiousBonus += (pEntry->GetFaithFromKills() / 5);
							}
							if (pEntry->GetCombatModifierEnemyCities() > 0 && (pkUnitEntry->GetUnitAIType(UNITAI_ATTACK) || pkUnitEntry->GetUnitAIType(UNITAI_RANGED) || pkUnitEntry->GetUnitAIType(UNITAI_SKIRMISHER) || pkUnitEntry->GetUnitAIType(UNITAI_FAST_ATTACK) || pkUnitEntry->GetUnitAIType(UNITAI_CITY_BOMBARD)))
							{
								iReligiousBonus += (pEntry->GetCombatModifierEnemyCities());
							}
							if (pEntry->GetCombatModifierFriendlyCities() > 0 && (pkUnitEntry->GetUnitAIType(UNITAI_DEFENSE) || pkUnitEntry->GetUnitAIType(UNITAI_COUNTER)))
							{
								iReligiousBonus += (pEntry->GetCombatModifierFriendlyCities());
							}
							if ((pEntry->GetCombatBonusVersusOtherReligionOwnLands() > 0 || pEntry->GetCombatBonusOwnLands() > 0) && (pkUnitEntry->GetUnitAIType(UNITAI_DEFENSE) || pkUnitEntry->GetUnitAIType(UNITAI_COUNTER)))
							{
								iReligiousBonus += (pEntry->GetCombatBonusVersusOtherReligionOwnLands() + pEntry->GetCombatBonusOwnLands()) * 2 / 3;
							}
							if ((pEntry->GetCombatBonusVersusOtherReligionTheirLands() > 0 || pEntry->GetCombatBonusTheirLands() > 0) && (pkUnitEntry->GetUnitAIType(UNITAI_ATTACK) || pkUnitEntry->GetUnitAIType(UNITAI_RANGED) || pkUnitEntry->GetUnitAIType(UNITAI_SKIRMISHER) || pkUnitEntry->GetUnitAIType(UNITAI_FAST_ATTACK) || pkUnitEntry->GetUnitAIType(UNITAI_CITY_BOMBARD)))
							{
								iReligiousBonus += (pEntry->GetCombatBonusVersusOtherReligionTheirLands() + pEntry->GetCombatBonusTheirLands()) * 2 / 3;
							}
							for (int iI = 0; iI < NUM_YIELD_TYPES; iI++)
							{
								const YieldTypes eYield = static_cast<YieldTypes>(iI);
								if (pEntry->GetYieldFromKills(eYield) > 0)
								{
									iReligiousBonus += (pEntry->GetYieldFromKills(eYield) / 5);
								}
							}
						}
					}
				}
			}
			iBonus += iReligiousBonus;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t028", 51, traceBonus, iBonus);
		}

		//////////////
		//CIVILIAN CHECKS
		//////////////////////

		if (pkUnitEntry->IsTrade())
		{
			//No if we're small.
			if (m_pCity->getPopulation() <= 4)
			{
				{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t029", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "CITY_POPULATION_TOO_LOW_FOR_TRADE_UNIT"); return traceResult; }
			}
			
			if (pkUnitEntry->GetDomainType() == DOMAIN_LAND)
			{
				iBonus += m_pCity->GetTradePriorityLand() * 10;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t029", 52, traceBonus, iBonus);
			}
			else
			{
				iBonus += m_pCity->GetTradePrioritySea() * 10;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t029", 53, traceBonus, iBonus);
			}

			//Less often if at war.
			if (bAtWar)
			{
				iBonus -= 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t029", 54, traceBonus, iBonus);
			}

			if (kPlayer.GetPlayerTraits()->IsDiplomat())
			{
				iBonus += 750;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t029", 55, traceBonus, iBonus);
			}
			else
			{
				iBonus += 700;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t029", 56, traceBonus, iBonus);
			}
		}

		if (pkUnitEntry->GetGoldenAgeTurns() != 0)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 57, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsTourism() || kPlayer.GetPlayerTraits()->IsExpansionist())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 58, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->GetBaseBeakersTurnsToCount() != 0)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 59, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsNerd())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 60, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->IsCultureBoost() != 0)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 61, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsTourism())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 62, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_MESSENGER)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 63, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsDiplomat())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 64, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->GetBaseGold() != 0)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 65, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsDiplomat())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 66, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->IsCanBuyCityState() != 0)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 67, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsDiplomat())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 68, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->GetBaseHurry() != 0)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 69, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsSmaller() || kPlayer.GetPlayerTraits()->IsWarmonger())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 70, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_GENERAL)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 71, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsExpansionist() || kPlayer.GetPlayerTraits()->IsWarmonger())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 72, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_ADMIRAL)
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 73, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsDiplomat() || kPlayer.GetPlayerTraits()->IsWarmonger())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 74, traceBonus, iBonus);
			}
		}
		if (pkUnitEntry->IsGreatWorkUnit())
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 75, traceBonus, iBonus);
			if (kPlayer.GetPlayerTraits()->IsTourism())
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t030", 76, traceBonus, iBonus);
			}
		}
	}

	//Settlers? Let's see...
	if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_SETTLE)
	{
		//Don't build a settler if we're about to grow.
		if (m_pCity->getFoodTurnsLeft() <= 1)
			{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t031", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "SETTLER_CITY_GROWTH_IMMINENT"); return traceResult; }

		//this checks war state, grand strategy and more!
		static EconomicAIStrategyTypes eNoMoreExpand = (EconomicAIStrategyTypes)GC.getInfoTypeForString("ECONOMICAISTRATEGY_ENOUGH_EXPANSION");
		if (GET_PLAYER(m_pCity->getOwner()).GetEconomicAI()->IsUsingStrategy(eNoMoreExpand))
			{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t031", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "ECONOMICAISTRATEGY_ENOUGH_EXPANSION"); return traceResult; }

		AICityStrategyTypes eEnoughSettlers = (AICityStrategyTypes)GC.getInfoTypeForString("AICITYSTRATEGY_ENOUGH_SETTLERS");
		if (m_pCity->GetCityStrategyAI()->IsUsingCityStrategy(eEnoughSettlers))
			{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t031", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "AICITYSTRATEGY_ENOUGH_SETTLERS"); return traceResult; }

		if (m_pCity->plot()->getNumUnitsOfAIType(UNITAI_SETTLE, m_pCity->getOwner()) > 0)
		{
			{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t031", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "SETTLER_ALREADY_IN_CITY"); return traceResult; }
		}

		int iFlavorExpansion = kPlayer.GetGrandStrategyAI()->GetPersonalityAndGrandStrategy((FlavorTypes)GC.getInfoTypeForString("FLAVOR_EXPANSION"));

		//we already checked ECONOMICAISTRATEGY_ENOUGH_EXPANSION, so we know we have a good settle plot
		//but let's bump the prio based on the quality of the plot
		int iQuality = kPlayer.GetSettlePlotQualityMeasure(kPlayer.GetBestSettlePlot(NULL));
		if (iQuality>0)
			iFlavorExpansion += iQuality;

		// If we are running "ECONOMICAISTRATEGY_EARLY_EXPANSION"
		if (kPlayer.IsEarlyExpansionPhase())
			//strategies affect unit flavors but unfortunately we largely ignore "pre" score from the flavor system
			iFlavorExpansion += 100;

		AICityStrategyTypes eFeeder = (AICityStrategyTypes)GC.getInfoTypeForString("AICITYSTRATEGY_NEW_CONTINENT_FEEDER");
		if (m_pCity->GetCityStrategyAI()->IsUsingCityStrategy(eFeeder))
			iFlavorExpansion += 50;

		if (kPlayer.GetPlayerTraits()->IsExpansionWLTKD())
			iFlavorExpansion += 10;

		//if we're even more unhappy then ECONOMICAISTRATEGY_ENOUGH_EXPANSION will kick in
		if (kPlayer.IsEmpireUnhappy())
			iFlavorExpansion -= 10;

		//if already we have more than 2 cities, let's try to get non-capital cities to be our settler-makers
		if (m_pCity->isCapital() && kPlayer.getNumCities() > 2)
		{
			iFlavorExpansion -= 25;
			//but this bonus may counteract
			iFlavorExpansion += kPlayer.getCapitalSettlerProductionModifier();
		}

		//sometimes it's just not right
		if (iFlavorExpansion <= 0)
			{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t031", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "NONPOSITIVE_ADJUSTED_EXPANSION_FLAVOR"); return traceResult; }

		//if we got this far we want to expand, so let's bump the number of times we've skipped this.
		iFlavorExpansion += kPlayer.GetMilitaryAI()->GetNumberOfTimesSettlerBuildSkippedOver() * 10;

		//bring it into a sensible range
		iBonus += iFlavorExpansion*23;
		if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t031", 85, traceBonus, iBonus);
	}

	if(!kPlayer.isMinorCiv())
	{
		//Archaeologists? Only if we have digs nearby.
		if(pkUnitEntry->GetDefaultUnitAIType() == UNITAI_ARCHAEOLOGIST)
		{
			static EconomicAIStrategyTypes eWantArch = (EconomicAIStrategyTypes) GC.getInfoTypeForString("ECONOMICAISTRATEGY_NEED_ARCHAEOLOGISTS");
			if(!kPlayer.GetEconomicAI()->IsUsingStrategy(eWantArch))
			{
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t032", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "ECONOMICAISTRATEGY_NEED_ARCHAEOLOGISTS;INACTIVE"); return traceResult; }
			}
			else
			{
				int iNumArch = kPlayer.GetNumUnitsWithUnitAI(UNITAI_ARCHAEOLOGIST, true);
				if (iNumArch <= 0)
				{
					iBonus += 5000;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t032", 86, traceBonus, iBonus);
				}
				
				if (kPlayer.GetDiplomacyAI()->IsGoingForCultureVictory())
				{
					iBonus += 5000;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t032", 87, traceBonus, iBonus);
				}

				if(kPlayer.GetArchaeologicalDigTourism() > 0)
				{
					iBonus += 2500;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t032", 88, traceBonus, iBonus);
				}
				for(int iI = 0; iI < NUM_YIELD_TYPES; iI++)
				{
					//Simplification - errata yields not worth considering.
					if ((YieldTypes)iI > YIELD_CULTURE_LOCAL && !MOD_BALANCE_CORE_JFD)
						break;

					const YieldTypes eYield = static_cast<YieldTypes>(iI);

					if(kPlayer.GetPlayerTraits()->GetArtifactYieldChanges(eYield) > 0)
					{
						iBonus += 500;
						if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t032", 89, traceBonus, iBonus);
					}
				}
			}
		}
	}

	//Make sure we need naval workers in this city.
	if(pkUnitEntry->GetDefaultUnitAIType() == UNITAI_WORKER_SEA)
	{
		CvTacticalDominanceZone* pZone = kPlayer.GetTacticalAI()->GetTacticalAnalysisMap()->GetZoneByCity(m_pCity,true);
		if (pZone && pZone->GetOverallDominanceFlag() != TACTICAL_DOMINANCE_FRIENDLY)
			{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t033", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "WORKBOAT_ZONE_NOT_FRIENDLY"); return traceResult; }

		CvPlayer& kPlayer = GET_PLAYER(m_pCity->getOwner());

		int iWorkBoatsNeeded = 0;
		int iClosestPlotDistance = MAX_INT;

		SPathFinderUserData data(m_pCity->getOwner(), PT_WORKER_SEA_UNIT_SAFE, 12);
		ReachablePlots allReachablePlots = GC.GetStepFinder().GetPlotsInReach(m_pCity->getX(), m_pCity->getY(), data);

		for (ReachablePlots::iterator it = allReachablePlots.begin(); it != allReachablePlots.end(); ++it)
		{
			CvPlot* pLoopPlot = GC.getMap().plotByIndexUnchecked(it->iPlotIndex);

			int iNumUnits = pLoopPlot->getNumUnits();
			for (int iI = 0; iI < iNumUnits; ++iI)
			{
				CvUnit* pLoopPlotUnit = pLoopPlot->getUnitByIndex(iI);
				// Check how many units are in the vicinity
				if (pLoopPlotUnit->AI_getUnitAIType() == UNITAI_WORKER_SEA)
					iWorkBoatsNeeded--;
			}

			CvCity* pLoopPlotCity = pLoopPlot->getPlotCity();
			if (pLoopPlotCity && (pLoopPlotCity != m_pCity || bForPurchase) && pLoopPlotCity->isProductionUnit())
			{
				CvUnitEntry* pkUnitEntry = GC.getUnitInfo(pLoopPlotCity->getProductionUnit());
				if (pkUnitEntry)
				{
					if (pkUnitEntry->GetDefaultUnitAIType() == UNITAI_WORKER_SEA)
					{
						iWorkBoatsNeeded--;
					}
				}
			}

			if (pLoopPlot->getImprovementType() != NO_IMPROVEMENT)
				continue;

			if (pLoopPlot->getOwner() != m_pCity->getOwner())
				continue;

			CvCity* pOwningCity = pLoopPlot->getOwningCity();
			if (!pOwningCity)
				continue;

			if (pOwningCity->IsRazing())
				continue;

			ResourceTypes eResource = pLoopPlot->getResourceType(kPlayer.getTeam());

			if (eResource == NO_RESOURCE)
				continue;

			if (!kPlayer.NeedWorkboatToImproveResource(eResource))
				continue;

			CvResourceInfo* pkResourceInfo = GC.getResourceInfo(eResource);
			if (pkResourceInfo && pkResourceInfo->getResourceUsage() == RESOURCEUSAGE_BONUS)
			{
				if (!pOwningCity->IsWithinWorkRange(pLoopPlot))
				{
					pOwningCity = pLoopPlot->getEffectiveOwningCity();
					if (!pOwningCity || !pOwningCity->IsWithinWorkRange(pLoopPlot))
						continue;
				}
			}

			int iPlotDistance = it->iPathLength;
			if (iPlotDistance < iClosestPlotDistance)
				iClosestPlotDistance = iPlotDistance;

			iWorkBoatsNeeded++;
		}

		if (iWorkBoatsNeeded > 0)
		{
			if (!bForPurchase)
			{
				iBonus += (1000 * iWorkBoatsNeeded * (m_pCity->getPopulation() + kPlayer.GetCurrentEra()));
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t033", 90, traceBonus, iBonus);
			}
			else
			{
				iBonus += (1000 * iWorkBoatsNeeded) * (kPlayer.getCapitalCity()->getPopulation() + kPlayer.GetCurrentEra()) - iClosestPlotDistance;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t033", 91, traceBonus, iBonus);
			}
		}
		else
			{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t033", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "NO_ELIGIBLE_WORKBOAT_TARGETS"); return traceResult; }
	}
	//Make sure we need workers in this city.
	if(pkUnitEntry->GetDefaultUnitAIType() == UNITAI_WORKER)
	{
		CvTacticalDominanceZone* pZone = kPlayer.GetTacticalAI()->GetTacticalAnalysisMap()->GetZoneByCity(m_pCity,false);
		if (pZone && pZone->GetOverallDominanceFlag() != TACTICAL_DOMINANCE_FRIENDLY)
			{ const int traceResult = SR_STRATEGY; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t034", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "WORKER_ZONE_NOT_FRIENDLY"); return traceResult; }

		//There's a worker waiting here? Abort!
		if (m_pCity->plot()->getNumUnitsOfAIType(UNITAI_WORKER, m_pCity->getOwner()) > 0)
			{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t034", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "WORKER_ALREADY_IN_CITY"); return traceResult; }

		int iNumBuilders = kPlayer.GetNumUnitsWithUnitAI(UNITAI_WORKER, true);
		int iMissingBuilders = kPlayer.getCitiesNeedingTerrainImprovements() + 1 - iNumBuilders; //one for roads
		int iScale = (iMissingBuilders > 2) ? 3 : 1; //escalate if many are missing

		static AICityStrategyTypes eNoWorkers = (AICityStrategyTypes)GC.getInfoTypeForString("AICITYSTRATEGY_ENOUGH_TILE_IMPROVERS");
		if (eNoWorkers != NO_AICITYSTRATEGY && m_pCity->GetCityStrategyAI()->IsUsingCityStrategy(eNoWorkers))
		{
			{ const int traceResult = SR_USELESS; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t034", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "AICITYSTRATEGY_ENOUGH_TILE_IMPROVERS"); return traceResult; }
		}
		static AICityStrategyTypes eWantWorkers = (AICityStrategyTypes)GC.getInfoTypeForString("AICITYSTRATEGY_WANT_TILE_IMPROVERS");
		if (eWantWorkers != NO_AICITYSTRATEGY && m_pCity->GetCityStrategyAI()->IsUsingCityStrategy(eWantWorkers))
		{
			iBonus += (200 * iMissingBuilders * iScale);
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t034", 92, traceBonus, iBonus);
		}
		static AICityStrategyTypes eNeedWorkers = (AICityStrategyTypes)GC.getInfoTypeForString("AICITYSTRATEGY_NEED_TILE_IMPROVERS");
		if (eNeedWorkers != NO_AICITYSTRATEGY && m_pCity->GetCityStrategyAI()->IsUsingCityStrategy(eNeedWorkers))
		{
			iBonus += (500 * iMissingBuilders * iScale);
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t034", 93, traceBonus, iBonus);
		}
	}
	
	if (!kPlayer.isMinorCiv())
	{
		//////////////////
		//WAR BOOSTERS
		////////////////////////
		int iWarBooster = 0;
		if (bCombat)
		{
			vector<PlayerTypes> enemies = kPlayer.GetPlayersAtWarWith();
			if (kPlayer.GetMilitaryAI()->GetWarType() == WARTYPE_LAND && eDomain == DOMAIN_LAND)
			{
				iWarBooster += enemies.size();
			}
			else if (kPlayer.GetMilitaryAI()->GetWarType() == WARTYPE_SEA && eDomain == DOMAIN_SEA)
			{
				iWarBooster += enemies.size();
			}

			iBonus += iWarBooster * 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t035", 94, traceBonus, iBonus);
		}
		if (bAtWar)
		{
			if (bCombat)
			{
				iBonus += m_pCity->getThreatValue();
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t035", 95, traceBonus, iBonus);
			}
			else
			{
				iBonus -= m_pCity->getThreatValue();
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t035", 96, traceBonus, iBonus);
			}
		}
		if (bCombat && bForOperation)
		{
			if (kPlayer.GetMilitaryAI()->GetNumberOfTimesOpsBuildSkippedOver() > 0)
			{
				iBonus += (kPlayer.GetMilitaryAI()->GetNumberOfTimesOpsBuildSkippedOver() * 100);
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t035", 97, traceBonus, iBonus);
			}
		}

		/////////////////////
		// EXPERIENCE BOOSTERS
		/////////////////////
		if (bCombat && kPlayer.getNumCities() > 1)
		{
			//Let's try to build our units in our best cities only.
			if (m_pCity == kPlayer.GetBestMilitaryCity((UnitCombatTypes)pkUnitEntry->GetUnitCombatType()))
			{
				iBonus += 25 * kPlayer.getNumCities();
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t036", 98, traceBonus, iBonus);
			}

			//Let's try to build our units in our best cities only.
			if (m_pCity == kPlayer.GetBestMilitaryCity(NO_UNITCOMBAT, pkUnitEntry->GetDomainType()))
			{
				iBonus += 25 * kPlayer.getNumCities();
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t036", 99, traceBonus, iBonus);
			}

		}

		//Promotion Bonus
		//consider performance, if bonus is very small it doesn't matter in the end
		int iPromotionBonus = 0;
		for(int iI = 0; iI < GC.getNumPromotionInfos() && bCombat; iI++)
		{
			const PromotionTypes ePromotion = static_cast<PromotionTypes>(iI);
			CvPromotionEntry* pkPromotionInfo = GC.getPromotionInfo(ePromotion);
			if(pkPromotionInfo)
			{
				// Promotions that are not lost usually mean unique units
				// but this also hits many negative or unit-line free promos.
				// So needs rewrite, but can probably just ignore!
				/*
				if (pkUnitEntry->GetFreePromotions(iI))
				{
					if (!pkPromotionInfo->IsLostWithUpgrade())
					{ 
						iPromotionBonus += 5;
					}
				}
				*/
				// pseudo unique units, e.g. Mongolia
				if(kPlayer.GetPlayerTraits()->HasFreePromotionUnitClass(iI, pkUnitEntry->GetUnitClassType()))
				{
					iPromotionBonus += 100;
				}
				// mostly accounted-for in flavors. etc. So small bonus?
				if(kPlayer.GetPlayerTraits()->HasFreePromotionUnitCombat(iI, pkUnitEntry->GetUnitCombatType()))
				{
					iPromotionBonus += 10;
				}
				// not currently used, but probably same as above
				if(kPlayer.IsFreePromotion(ePromotion))
				{
					iPromotionBonus += 10;
				}
				// in case someone adds many promos with traits, do not let this loop blow up
				// also, if we find a "pseudo unique unit" we can stop looping
				if (iPromotionBonus >= 100)
					break;
			}
		}
		if (iPromotionBonus != 0)
		{
			iBonus += iPromotionBonus;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t037", 103, traceBonus, iBonus);
		}
	
		//Uniques? They're generally good enough to spam.
		if(kPlayer.getCivilizationInfo().isCivilizationUnitOverridden(pkUnitEntry->GetUnitClassType()))
		{
			iBonus += 100;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t037", 104, traceBonus, iBonus);
		}

		//For an operation? Build it!
		if(bCombat)
		{
			if(bForOperation)
			{
				iBonus += 500;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t038", 105, traceBonus, iBonus);
			}

			//slight bump in value for ranged units, to counteract some other elements.
			if (pkUnitEntry->GetRangedCombat() > 0)
			{
				iBonus += pkUnitEntry->GetRangedCombat();
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t038", 106, traceBonus, iBonus);
			}

			if (kPlayer.IsUnderrepresentedUnitType(pkUnitEntry->GetDefaultUnitAIType()))
			{
				iBonus += 200;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t038", 107, traceBonus, iBonus);
			}

			if (eDomain == DOMAIN_LAND)
			{
				CvUnit* pGarrison = m_pCity->GetGarrisonedUnit();
				if (!pGarrison || pGarrison->getDomainType() != DOMAIN_LAND)
				{
					iBonus += 25;
					if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t038", 108, traceBonus, iBonus);
				}
			}
		}
	}

	//Tiny army? Eek!
	if (iNumLandUnits <= (kPlayer.getNumCities() * 2) && pkUnitEntry->GetDomainType() == DOMAIN_LAND)
	{
		if(bCombat)
		{
			iBonus += 300;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t039", 109, traceBonus, iBonus);
		}
		//Fewer civilians til we rectify this!
		else
		{
			iBonus -= 300;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t039", 110, traceBonus, iBonus);
		}
	}

	//Tiny navy? Eek!
	if (iNumSeaUnits <= kPlayer.GetNumEffectiveCoastalCities() && pkUnitEntry->GetDomainType() == DOMAIN_SEA)
	{
		if (bCombat)
		{
			iBonus += 300;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t039", 111, traceBonus, iBonus);
		}
		//Fewer civilians til we rectify this!
		else
		{
			iBonus -= 300;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t039", 112, traceBonus, iBonus);
		}
	}
	
	if (bDesperate)
	{
		if (bCombat)
		{
			iBonus += 500;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t039", 113, traceBonus, iBonus);
		}
		else
		{
			iBonus -= 500;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t039", 114, traceBonus, iBonus);
		}
	}

	if (bCombat)
	{
		static MilitaryAIStrategyTypes eStrategyBarbs = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ERADICATE_BARBARIANS");
		static MilitaryAIStrategyTypes eStrategyBarbsCritical = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ERADICATE_BARBARIANS_CRITICAL");
		
		if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyBarbs))
		{
			iBonus += 250;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t040", 115, traceBonus, iBonus);
		}
		else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyBarbsCritical))
		{
			iBonus += 500;
			if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t040", 116, traceBonus, iBonus);
		}
	}

	if (bCombat && !bForPurchase)
	{
		UnitTypes eCurrentUnit = m_pCity->getProductionUnit();
		//making a combat unit?
		if (eCurrentUnit != NO_UNIT)
		{
			//working on this unit? finish up if at war.
			if (eUnit == eCurrentUnit && bAtWar)
			{
				iBonus += 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t040", 117, traceBonus, iBonus);
			}
			//a little boost if not at war (to finish)
			else if (eUnit == eCurrentUnit && !bAtWar)
			{
				iBonus += 50;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t040", 118, traceBonus, iBonus);
			}
			// looking at another unit? let's not switch.
			else if (GC.getUnitInfo(eCurrentUnit)->GetCombat() > 0 || GC.getUnitInfo(eCurrentUnit)->GetRangedCombat() > 0)
			{
				iBonus -= 100;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_SUB("t040", 119, traceBonus, iBonus);
			}
		}
	}

	// Add a boost to score proportional to instant yield gains when the unit is completed
	int iInstantYieldImportanceScale = 1; // Relative importance of instant yields obtained to score bonus
	int iInstantYieldBonus = 0;

	for (int iI = 0; iI < NUM_YIELD_TYPES; iI++)
	{
		const YieldTypes eYield = static_cast<YieldTypes>(iI);
		int iYieldAmountOnComplete = GC.getUnitInfo(eUnit)->GetYieldOnCompletion(eYield);
		if (iYieldAmountOnComplete > 0)
		{
			iInstantYieldBonus += (iYieldAmountOnComplete * iInstantYieldImportanceScale);
		}
	}

	iBonus += iInstantYieldBonus;
	if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t041", 121, traceBonus, iBonus);

	//Make sure that free units are given some kind of value.
	if (bFree && iBonus <= iFlavorWeight)
	{
		iBonus = iFlavorWeight;
		if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ASSIGN("t041", 122, traceBonus, iBonus);
	}

	/*
	if (!kPlayer.isMinorCiv() && !pkUnitEntry->IsNoSupply())
	{
		if (bCombat && !bFree)
		{
			//Let's check this against supply to keep our military numbers lean.
			int iScale = 100;
			int iSupply = max(1, kPlayer.GetNumUnitsSupplied());
			int iDemand = iNumTotalUnitsToSupply;

			//don't exceed the limit
			if (iSupply <= iDemand)
				return SR_UNITSUPPLY;

			if (!bAtWar && !bForOperation)
			{
				//reduce bonus once we're approaching the limit (scale is 50% if at max supply)
				iScale = MapToPercent(iDemand, (iSupply * 4) / 3, (iSupply * 2) / 3);
			}

			if (iScale != 100)
			{
				iBonus *= iScale;
				iBonus /= 100;
			}
		}
	}
	*/

	//prioritize besieged cities for purchase
	if (bForPurchase && bCombat && bDesperate)
	{
		iBonus += 250;
		if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t041", 123, traceBonus, iBonus);
	}

	// MOD_AI_UNIT_PRODUCTION - NEW STRATEGIES -- consider if we have too much or too few of a unit
	if (bCombat && MOD_AI_UNIT_PRODUCTION)
	{
		static int iBonusValueFromStrategy = 100;

		static MilitaryAIStrategyTypes eStrategyArcher = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_ARCHER");
		static MilitaryAIStrategyTypes eStrategyEnoughArcher = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_ARCHER");
		static MilitaryAIStrategyTypes eStrategySiege = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_SIEGE");
		static MilitaryAIStrategyTypes eStrategyEnoughSiege = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_SIEGE");
		static MilitaryAIStrategyTypes eStrategySkirmisher = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_SKIRMISHER");
		static MilitaryAIStrategyTypes eStrategyEnoughSkirmisher = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_SKIRMISHER");
		static MilitaryAIStrategyTypes eStrategyMobile = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_MOBILE");
		static MilitaryAIStrategyTypes eStrategyEnoughMobile = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_MOBILE");
		static MilitaryAIStrategyTypes eStrategyNavalMelee = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_NAVAL_MELEE");
		//static MilitaryAIStrategyTypes eStrategyEnoughNavalMelee = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_NAVAL_MELEE");
		static MilitaryAIStrategyTypes eStrategyNavalRanged = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_NAVAL_RANGED");
		static MilitaryAIStrategyTypes eStrategyEnoughNavalRanged = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_NAVAL_RANGED");
		static MilitaryAIStrategyTypes eStrategySubmarine = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_SUBMARINE");
		static MilitaryAIStrategyTypes eStrategyEnoughSubmarine = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_SUBMARINE");
		static MilitaryAIStrategyTypes eStrategyBomber = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_BOMBER");
		static MilitaryAIStrategyTypes eStrategyEnoughBomber = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_BOMBER");
		static MilitaryAIStrategyTypes eStrategyFighter = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_NEED_FIGHTER");
		static MilitaryAIStrategyTypes eStrategyEnoughFighter = (MilitaryAIStrategyTypes)GC.getInfoTypeForString("MILITARYAISTRATEGY_ENOUGH_FIGHTER");

		switch (pkUnitEntry->GetDefaultUnitAIType())
		{
		case UNITAI_RANGED:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyArcher))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 124, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughArcher))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_ARCHER"); return traceResult; }
			break;
		case UNITAI_CITY_BOMBARD:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategySiege))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 125, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughSiege))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_SIEGE"); return traceResult; }
			break;
		case UNITAI_SKIRMISHER:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategySkirmisher))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 126, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughSkirmisher))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_SKIRMISHER"); return traceResult; }
			break;
		case UNITAI_FAST_ATTACK:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyMobile))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 127, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughMobile))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_MOBILE"); return traceResult; }
			break;
		case UNITAI_ATTACK_SEA:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyNavalMelee))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 128, traceBonus, iBonus);
			}
			/*else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughNavalMelee))
				return SR_BALANCE;*/ // ranged naval units may be resource limited, so don't discourage these
			break;
		case UNITAI_ASSAULT_SEA:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyNavalRanged))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 129, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughNavalRanged))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_NAVAL_RANGED"); return traceResult; }
			break;
		case UNITAI_SUBMARINE:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategySubmarine))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 130, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughSubmarine))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_SUBMARINE"); return traceResult; }
			break;
		case UNITAI_ATTACK_AIR:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyBomber))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 131, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughBomber))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_BOMBER"); return traceResult; }
			break;
		case UNITAI_DEFENSE_AIR:
			if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyFighter))
			{
				iBonus += iBonusValueFromStrategy;
				if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t042", 132, traceBonus, iBonus);
			}
			else if (kPlayer.GetMilitaryAI()->IsUsingStrategy(eStrategyEnoughFighter))
				{ const int traceResult = SR_BALANCE; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t042", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "MILITARYAISTRATEGY_ENOUGH_FIGHTER"); return traceResult; }
			break;
		default:
			break;
		}
	}

	/////
	///WEIGHT
	//////

	if (iBonus < 0)
		{ const int traceResult = SR_UNKNOWN; if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t043", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, "NEGATIVE_SANITY_BONUS"); return traceResult; }

	//iValue modified by iBonus
	iFlavorWeight += iBonus;
	if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_ADD("t043", 133, traceFlavorWeight, iFlavorWeight);

	if (m_pCity->IsOccupied() && !m_pCity->IsNoOccupiedUnhappiness())
	{
		iFlavorWeight /= 5;
		if (MOD_AI_UNIT_DECISION_LOGGING) AI_UNIT_TRACE_DIV("t043", 134, traceFlavorWeight, iFlavorWeight);
	}

	{ const int traceResult = max(1,iFlavorWeight); if (AIUnitDecisionTrace::Enabled()) AIUnitDecisionTrace::Event("RETURN", "t043", m_pCity->getOwner(), m_pCity->GetID(), -1, eUnit, iBonus, traceResult, ""); return traceResult; }
}


/// Log all potential builds
void CvUnitProductionAI::LogPossibleBuilds(UnitAITypes eUnitAIType)
{
	if(GC.getLogging() && GC.getAILogging())
	{
		// Find the name of this civ and city
		CvString playerName = GET_PLAYER(m_pCity->getOwner()).getCivilizationShortDescription();
		CvString cityName = m_pCity->getName();

		// Open the log file
		FILogFile* pLog = LOGFILEMGR.GetLog(m_pCity->GetCityStrategyAI()->GetLogFileName(playerName, cityName), FILogFile::kDontTimeStamp);

		// Get the leading info for this line
		CvString strBaseString;
		strBaseString.Format("%03d, ", GC.getGame().getElapsedGameTurns());
		strBaseString += playerName + ", " + cityName + ", ";


		// Dump out the weight of each buildable item
		for(int iI = 0; iI < m_Buildables.size(); iI++)
		{
			CvString strOutBuf = strBaseString;

			CvUnitEntry* pUnitEntry = GC.GetGameUnits()->GetEntry(m_Buildables.GetElement(iI));

			CvString strDesc = (pUnitEntry != NULL)? pUnitEntry->GetDescription() : "Unknown Unit";
			if(eUnitAIType == NO_UNITAI)
			{
				CvString strTemp;
				strTemp.Format("Unit, %s, %d", strDesc.GetCString(), m_Buildables.GetWeight(iI));
				strOutBuf += strTemp;
			}
			else
			{
				CvString strTempString;
				getUnitAIString(strTempString, eUnitAIType);

				CvString strTemp;
				strTemp.Format("Special request for unit of type: %s, %s, %d", strTempString.GetCString(), strDesc.GetCString(), m_Buildables.GetWeight(iI));
				strOutBuf += strTemp;
			}

			pLog->Msg(strOutBuf);
		}
	}
}
