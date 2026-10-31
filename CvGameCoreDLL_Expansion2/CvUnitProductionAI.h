/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#pragma once

#ifndef CIV5_UNIT_PRODUCTION_AI_H
#define CIV5_UNIT_PRODUCTION_AI_H

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//  CLASS:      CvUnitProductionAI
//!  \brief		Handles unit production decisions for one city
//
//!  Key Attributes:
//!  - Object created by CvCityStrategyAI (so one per city)
//!  - Uses flavor-based weighting choices (similar to one used for policies and techs)
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "AIUnitDecisionTrace.h"
class CvUnitProductionAI
{
public:
	CvUnitProductionAI(CvCity* pCity, CvUnitXMLEntries* pUnits);
	~CvUnitProductionAI(void);

	// Initialization
	void Reset();

	// Serialization routines
	template<typename UnitProductionAI, typename Visitor>
	static void Serialize(UnitProductionAI& unitProductionAI, Visitor& visitor);
	void Read(FDataStream& kStream);
	void Write(FDataStream& kStream) const;

	// Establish/retrieve weights for one flavor
	void AddFlavorWeights(FlavorTypes eFlavor, int iWeight);
	int GetWeight(UnitTypes eUnit);
	void TraceWeight(UnitTypes eUnit, int weight);
	void TraceConsumeFlavorState();
	void TraceFlavorComponents(int flavor, int effective, const AIUnitDecisionTrace::Components& parts, bool available);
	void TraceFlavorUpdateComplete();

	// Recommend highest-weighted unit
	int CheckUnitBuildSanity(UnitTypes eUnit, bool bForOperation, int iFlavorWeight, bool bForPurchase = false, bool bFree = false);
	UnitTypes RecommendUnit(UnitAITypes eUnitAIType, bool bAllowStrategicResource);

	// Logging
	void LogPossibleBuilds(UnitAITypes eUnitAIType = NO_UNITAI);

private:

	// Private data
	CvCity* m_pCity;
	CvUnitXMLEntries* m_pUnits;
	struct FlavorPart { int flavor, effective, coefficient; FlavorPart(int f,int e,int c):flavor(f),effective(e),coefficient(c) {} };
	std::vector<std::vector<FlavorPart> > m_traceFlavors;
	bool m_traceFlavorsComplete;
	int m_traceFlavorState;
	std::vector<AIUnitDecisionTrace::Components> m_traceComponents;
	std::vector<bool> m_traceComponentsAvailable;
	std::vector<int> m_traceFlavorValues, m_tracePreviousValues;
	std::vector<AIUnitDecisionTrace::Components> m_tracePreviousComponents;
	std::vector<bool> m_tracePreviousAvailable;
	int m_tracePreviousState;
	CvWeightedVector<int> m_UnitAIWeights;
	CvWeightedVector<int> m_Buildables;
};

FDataStream& operator>>(FDataStream&, CvUnitProductionAI&);
FDataStream& operator<<(FDataStream&, const CvUnitProductionAI&);

#endif //CIV5_UNIT_PRODUCTION_AI_H