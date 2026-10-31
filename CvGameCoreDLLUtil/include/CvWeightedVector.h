#pragma once

#ifndef CIV5_WEIGHTED_VECTOR_H
#define CIV5_WEIGHTED_VECTOR_H

#include <vector>
#include <algorithm>
#include "EventSystem/FastDelegate.h"
#include "CvRandom.h"

// Functor for random number callback routine
typedef fastdelegate::FastDelegate2<int, const char*, int> RandomNumberDelegate;

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//  CLASS:     CvWeightedVector
//!  \brief	   Container holding objects with associated weighting
//
//!  Key Attributes:
//!  - Underlying data structure is FFastVector from Fireworks
//!  - See documentation on template parameters from FFastVector; same ones used here
//!  - Main purpose of class is use for weighted AI selection
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
template<class T> class CvWeightedVector
{
public:
	/// One element stored in our weighted vector
	struct WeightedElement
	{
		WeightedElement() : 
		m_Element()
		, m_iWeight(0)
		{
		}

		WeightedElement(const T& e, int w) : 
		m_Element(e)
		, m_iWeight(w)
		{
		}

		WeightedElement(const WeightedElement & source) :
		m_Element(source.m_Element)
		, m_iWeight(source.m_iWeight)
		{
		}

		T m_Element;
		int m_iWeight;

		bool operator< (const WeightedElement& b2) const
		{
			// Reverse of the normal direction because we want highest weight first in our list
			return m_iWeight > b2.m_iWeight;
		};

	};

	/// Default constructor
	CvWeightedVector(void)
	{
	};

	/// Destructor
	~CvWeightedVector(void)
	{
	};

	/// Accessor for element
	const T& GetElement (unsigned int iIndex) const
	{
		ASSERT(iIndex < m_items.size());
		return m_items[iIndex].m_Element;
	};

	/// Accessor for element
	void SetElement (unsigned int iIndex, const T& iValue)
	{
		ASSERT(iIndex < m_items.size());
		m_items[iIndex].m_Element = iValue;
	};

	/// Accessors for weight
	int GetWeight (unsigned int iIndex) const
	{
		ASSERT(iIndex < m_items.size());
		return m_items[iIndex].m_iWeight;
	}
	void SetWeight (unsigned int iIndex, int iWeight)
	{
		ASSERT(iWeight >= 0);
		m_items[iIndex].m_iWeight = iWeight;
	}
	void IncreaseWeight (unsigned int iIndex, int iWeight)
	{
		SetWeight(iIndex, GetWeight(iIndex) + iWeight);
	}

	/// Return total of all weights stored in vector
	int GetTotalWeight () const
	{
		int rtnValue = 0;

		for (unsigned int i = 0; i < m_items.size(); i++)
		{
			WeightedElement elem = m_items[i];
			rtnValue += elem.m_iWeight;
			ASSERT(rtnValue >= 0);
		}

		return rtnValue;
	};

	/// Add an item to the end of the vector
	unsigned int push_back (const T& element, int iWeight)
	{
		ASSERT(iWeight >= 0);
		m_items.push_back(WeightedElement(element,iWeight));
		return m_items.size();
	};

	/// Clear out the vector
	void clear ()
	{
		m_items.clear();
	};

	/// Resize the vector
	void resize (uint uiNewSize)
	{
		m_items.resize(uiNewSize);
	};

	/// Set the vector reserve
	void reserve (uint uiNewSize)
	{
		m_items.reserve(uiNewSize);
	};

	/// Number of items
	int size () const
	{
		return m_items.size();
	};

	bool empty () const
	{
		return m_items.size() == 0;
	}

	/// Sort this stuff from highest to lowest
	void SortItems ()
	{
		std::sort(m_items.begin(), m_items.end());
	}

	void StableSortItems()
	{
		std::stable_sort(m_items.begin(), m_items.end());
	}

	/// Return a random entry by weight, but avoid unlikely candidates (by only looking at candidates with a certain percentage of the top score)
	T ChooseAbovePercentThreshold(int iPercent, CvSeeder seed, void (*trace)(const char*, int, int, int) = NULL)
	{
		iPercent = range(iPercent, 0, 100);
		if (trace) trace("THRESHOLD_PERCENT", iPercent, (int)m_items.size(), 0);

		// First, calculate the highest weight
		int iHighestWeight = 0;
		for (unsigned int i = 0; i < m_items.size(); i++)
		{
			WeightedElement elem = m_items[i];
			if (elem.m_iWeight >= iHighestWeight)
			{
				iHighestWeight = elem.m_iWeight;
			}
		}

		// If nothing has any weight, choose at random
		if (iHighestWeight == 0)
		{
			if (trace) trace("MODE_UNIFORM", (int)m_items.size(), 0, 0);
			return ChooseAtRandom(seed, trace);
		}
		// 0% = consider all choices, with no cutoff.
		else if (iPercent == 0)
		{
			if (trace) trace("MODE_NO_CUTOFF", (int)m_items.size(), 0, 0);
			return ChooseByWeight(seed, trace);
		}

		// Compute cutoff for the requested percentage
		int iCutoff = iHighestWeight * iPercent / 100;
		if (trace) trace("CUTOFF", iPercent, iHighestWeight, iCutoff);

		// Create a temporary vector with only the items that meet the cutoff
		CvWeightedVector<T> tempVector;
		for (unsigned int i = 0; i < m_items.size(); i++)
		{
			WeightedElement elem = m_items[i];
			if (trace) trace("POOL", (int)i, elem.m_iWeight, iCutoff);
			if (elem.m_iWeight >= iCutoff)
			{
				tempVector.push_back(elem.m_Element, elem.m_iWeight);
			}
		}

		// Make the choice!
		return tempVector.ChooseByWeight(seed, trace);
	};

	/// Return a random entry (ignoring weight)
	T ChooseAtRandom(CvSeeder seed, void (*trace)(const char*, int, int, int) = NULL)
	{
		// Based on the number of elements we have, pick one at random
		uint uChoice = GC.getGame().urandLimitExclusive(m_items.size(), seed);
		if (trace) {
			for (unsigned int i=0; i<m_items.size(); ++i) trace("ELIGIBLE", (int)i, m_items[i].m_iWeight, (int)m_items.size());
			trace("SELECTED_INDEX", (int)uChoice, (int)m_items.size(), 0);
		}
		if (trace) trace("UNIFORM_DRAW", (int)uChoice, (int)m_items.size(), 0);
		return m_items[uChoice].m_Element;
	};

	/// Choose by weight (even considering unlikely candidates, but not those with zero weight)
	T ChooseByWeight(CvSeeder seed, void (*trace)(const char*, int, int, int) = NULL)
	{
		// If nothing has any weight, choose at random
		int iTotalWeight = GetTotalWeight();
		if (iTotalWeight == 0)
			return ChooseAtRandom(seed, trace);

		// Random roll up to total weight
		if (trace) {
			for (unsigned int i=0; i<m_items.size(); ++i) trace("ELIGIBLE", (int)i, m_items[i].m_iWeight, iTotalWeight);
		}
		int iChoice = GC.getGame().randRangeExclusive(0, iTotalWeight, seed);
		if (trace) trace("WEIGHTED_DRAW", iChoice, iTotalWeight, 0);

		// Loop through until we find the item that is in the range for this roll
		for (unsigned int i = 0; i < m_items.size(); i++)
		{
			WeightedElement elem = m_items[i];
			iChoice -= elem.m_iWeight;
			if (trace) trace("BOUNDARY", (int)i, elem.m_iWeight, iChoice);
			if (iChoice < 0) {
				if (trace) trace("SELECTED_INDEX", (int)i, elem.m_iWeight, iChoice);
				return elem.m_Element;
			}
		}

		// We should have found something, so reaching here is an error
		UNREACHABLE();
	};

	/// Pick an element from the top iNumChoices
	T ChooseFromTopChoices(int iNumChoices, CvSeeder seed)
	{
		ASSERT(iNumChoices > 0);

		// the easy case
		if (iNumChoices == 1)
			return m_items[0].m_Element;

		// Loop through the top choices, or the total vector size, whichever is smaller
		if (iNumChoices > (int) m_items.size())
			iNumChoices = (int) m_items.size();

		// Get the total weight
		int iTotalTopChoicesWeight = 0;
		for (int i = 0; i < iNumChoices; i++)
		{
			iTotalTopChoicesWeight += m_items[i].m_iWeight;
		}

		// All of the choices have a weight of 0 so pick randomly
		if (iTotalTopChoicesWeight == 0)
			return ChooseAtRandom(seed);

		int iChoice = GC.getGame().randRangeExclusive(0, iTotalTopChoicesWeight, seed);

		// Find out which element was chosen
		for (int i = 0; i < iNumChoices; i++)
		{
			WeightedElement elem = m_items[i];
			iChoice -= elem.m_iWeight;
			if (iChoice < 0)
				return elem.m_Element;
		}

		// We should have found something, so reaching here is an error
		UNREACHABLE();
	};

private:
	std::vector<WeightedElement> m_items;
};

//-------------------------------
// Serialization helper templates:
//-------------------------------

template<typename T>
FDataStream & operator<<(FDataStream & writeTo, const CvWeightedVector<T> & readFrom)
{
	writeTo << readFrom.size();
	for (int i = 0; i < readFrom.size(); i++)
	{
		writeTo << readFrom.GetElement(i);
		writeTo << readFrom.GetWeight(i);
	}
	return writeTo;
}

template<typename T>
FDataStream & operator>>(FDataStream & readFrom, CvWeightedVector<T> & writeTo)
{
	int nItems;
	readFrom >> nItems;

	writeTo.clear();
	for (int i=0; i<nItems; i++)
	{
		T e;
		int w;
		readFrom >> e;
		readFrom >> w;
		writeTo.push_back(e,w);
	}
	return readFrom;
}

#endif //CIV5_WEIGHTED_VECTOR_H