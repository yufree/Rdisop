// RcppExample.cpp: Part of the R/C++ interface class library, Version 4.2
//
// Copyright (C) 2005-2006 Dominick Samperi
//
// This library is free software; you can redistribute it and/or modify it 
// under the terms of the GNU Lesser General Public License as published by 
// the Free Software Foundation; either version 2.1 of the License, or (at 
// your option) any later version.
//
// This library is distributed in the hope that it will be useful, but 
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public 
// License for more details.
//
// You should have received a copy of the GNU Lesser General Public License 
// along with this library; if not, write to the Free Software Foundation, 
// Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA 

// {{{ Includes, Typedefs, prototypes

#include <iostream>
#include <iomanip>
#include <vector>
#include <functional>
#include <algorithm>
#include <map>
#include <sstream>
#include <numeric>
#include <string>
#include <cstring>

//
// IMS Stuff
//
#include <ims/alphabet.h>
#include <ims/weights.h>
#include <ims/isotopedistribution.h>
#include <ims/distributionprobabilityscorer.h>
#include <ims/composedelement.h>
#include <ims/nitrogenrulefilter.h>
#include <ims/utils/math.h>
#include <ims/base/exception/ioexception.h>
#include <ims/decomp/realmassdecomposer.h>
#include <ims/decomp/integermassdecomposer.h>
#include <ims/decomp/decomputils.h>

//
// R Stuff
//
#include "../RcppSrc/Rcpp.hpp"
extern "C" {
#include <Rdefines.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
}

using namespace ims;
using namespace std;

typedef Alphabet alphabet_t;
typedef IsotopeDistribution distribution_t;
typedef IntegerMassDecomposer<>::decompositions_type decompositions_t;

void initializeCHNOPS(alphabet_t&, 
		      const int maxisotopes);
void initializeAlphabet(const SEXP l_alphabet, 
			alphabet_t &alphabet, 
			const int maxisotopes);

template <typename score_type>
SEXP  rlistScores(multimap<score_type, ComposedElement, greater<score_type> > scores, int z);

// }}}

         
template <typename MassType>
SEXP rlistDecompositions(const decompositions_t& decompositions, 
			  const alphabet_t& alphabet, const Weights& weights,
			  MassType mass, unsigned int maxNumber);

char* exceptionMesg=NULL;

float getDBE(const ComposedElement& molecule, int z) {
  // {{{ 

  return (1 + static_cast<int>(molecule.getElementAbundance("C"))
	  + static_cast<int>(molecule.getElementAbundance("Si"))
	  - 0.5 * (static_cast<int>(molecule.getElementAbundance("H"))
		   +static_cast<int>(molecule.getElementAbundance("F"))
		   +static_cast<int>(molecule.getElementAbundance("Cl"))
		   +static_cast<int>(molecule.getElementAbundance("Br"))
		   +static_cast<int>(molecule.getElementAbundance("I")))
	  + 0.5 * (static_cast<int>(molecule.getElementAbundance("N"))
		   + static_cast<int>(molecule.getElementAbundance("P"))));
};

// }}}


char getParity(const ComposedElement& molecule, int charge=0) {
  // {{{ 

  //  return (int)(getDBE(molecule, z) * 2) % 2 == 0 ? 'e' : 'o'; 

  bool masseven =  static_cast<int>(molecule.getMass()) % 2 == 0 ? true : false;
  bool nitrogeneven = static_cast<int>(molecule.getElementAbundance("N")) % 2 == 0 ? true : false;
  bool chargeeven = abs(charge) % 2 == 0 ? true : false;
  
  return (masseven ^ nitrogeneven ^ chargeeven) == true ? 'e' : 'o' ;
};

// }}}

bool isValidMyNitrogenRule(const ComposedElement& molecule, int z) {
  // {{{ 

  bool massodd =  static_cast<int>(molecule.getNominalMass()) % 2 == 1 ? true : false;
  bool masseven = !massodd;

  bool nitrogenodd = static_cast<int>(molecule.getElementAbundance("N")) % 2 == 1 ? true : false;
  bool nitrogeneven = !nitrogenodd;

  bool parityodd = getParity(molecule, z) == 'o' ? true : false;
  bool parityeven = !parityodd;

  bool zodd = z % 2 == 1 ? true : false;
  bool zeven = !zodd;

  return (  zeven & masseven & nitrogeneven )
    |    (  zeven & massodd  & nitrogenodd  )
    |    (  zodd  & masseven & nitrogenodd  )
    |    (  zodd  & massodd  & nitrogeneven );

}
// }}}


//
// Decomposition of Mass / Isotope Pattern
//

RcppExport SEXP decomposeIsotopes(SEXP v_masses, SEXP v_abundances, SEXP s_error, 
				  SEXP l_alphabet, SEXP v_element_order, 
				  SEXP z, SEXP i_maxisotopes) {
// {{{ 

    typedef DistributionProbabilityScorer scorer_type;
    typedef scorer_type::score_type score_type;
    typedef scorer_type::masses_container masses_container;
    typedef scorer_type::abundances_container abundances_container;
    typedef distribution_t::peaks_container peaks_container;
    typedef distribution_t::mass_type mass_type;
    typedef distribution_t::abundance_type abundance_type;
    typedef distribution_t::nominal_mass_type nominal_mass_type;
    typedef multimap<score_type, ComposedElement, greater<score_type> > scores_container;
    typedef vector<pair<ComposedElement, score_type> > nonnormalized_scores_container;
    typedef decompositions_t::value_type decomposition_type;

    // Reset error state
    exceptionMesg = NULL;

    SEXP  rl=R_NilValue; // Use this when there is nothing to be returned.
    try {

	RcppVector<double> masses = RcppVector<double>(v_masses);
	RcppVector<double> abundances = RcppVector<double>(v_abundances);
	double error = *REAL(s_error);

	// converts relative (ppm) in absolute error 
	error *= masses(0) * 1.0e-06;

	// initializes precision
	double precision = 1.0e-05;
	int number_molecules_shown = 100;
	
	// initializes alphabet
	int maxisotopes = INTEGER_VALUE(i_maxisotopes);
	alphabet_t alphabet;
	vector<string> elements_order;

	if (l_alphabet == NULL || length(l_alphabet) < 1  ) {
	  initializeCHNOPS(alphabet, maxisotopes); 
	  // initializes order of atoms in which one would
	  // like them to appear in the molecules sequence
	  elements_order.push_back("C");
	  elements_order.push_back("H");
	  elements_order.push_back("N");
	  elements_order.push_back("O");
	  elements_order.push_back("P");
	  elements_order.push_back("S");
	} else {
	  initializeAlphabet(l_alphabet, alphabet, maxisotopes);
	  
	  int element_length = length(v_element_order);
	  for (int i=0; i<element_length; i++) {
	    elements_order.push_back(string(CHAR(STRING_ELT(v_element_order,i))));
	  }
	}
	
	// initializes weights
	Weights weights(alphabet.getMasses(), precision);
	
	// checks if weights could become smaller, by dividing on gcd.
	weights.divideByGCD();
	
	// initializes decomposer
	RealMassDecomposer decomposer(weights);
	
	// normalizes abundances
	abundance_type abundances_sum = 0.0;
	for (peaks_container::size_type i = 0; i < abundances.size(); ++i) {
		abundances_sum += abundances(i);
	}
	for (peaks_container::size_type i = 0; i < abundances.size(); ++i) {
		abundances(i) /= abundances_sum;
	}

	// fills peaklist masses and abundances, 
	// since we cannot use masses and abundances - instances of RcppVector object - directly
	masses_container peaklist_masses;
	abundances_container peaklist_abundances;
	for (masses_container::size_type mi = 0; mi < masses.size() && mi < abundances.size(); ++mi) {
		peaklist_masses.push_back(masses(mi));
		peaklist_abundances.push_back(abundances(mi));
	}

	// initializes distribution probability scorer
	scorer_type scorer(peaklist_masses, peaklist_abundances);
	
	// initializes storage to store sum formulas and their non-normalized scores
	nonnormalized_scores_container nonnormalized_scores;

	// initializes storage for results: sum formulas and their scores
	scores_container scores;

	///////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////  Start identification pipeline /////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////

	// gets all possible decompositions for the monoisotopic mass with error allowed
	decompositions_t decompositions = 
		decomposer.getDecompositions(masses(0), error);

	score_type accumulated_score = 0.0;	
	// for every decomposition:
	// - chemical filter is applied
	// - isotopic pattern is calculated
	// - isotopic pattern is matched against input spectrum
	for (decompositions_t::iterator decomps_it = decompositions.begin(); 
		decomps_it != decompositions.end(); ++decomps_it) {

		// creates a candidate molecule out of elemental composition and a set of elements
		ComposedElement candidate_molecule(*decomps_it, alphabet);
		// checks on chemical filter
// 		if (!isValidMyNitrogenRule(candidate_molecule, z)) {
// 			continue;
// 		} 
		// updates molecules isotope distribution (since its not calculated upon creation: 
		// it would be time consuming before applying chemical filter)
		candidate_molecule.updateIsotopeDistribution();
		// updates molecules sequence in a order of elements(atoms) one would like it
		// to appear
		candidate_molecule.updateSequence(&elements_order);

		// gets a theoretical isotope distribution of the candidate molecule
		IsotopeDistribution candidate_molecule_distribution = 
				candidate_molecule.getIsotopeDistribution();
		
		// gets a sequence of the candidate molecule
		string candidate_molecule_sequence = 
				candidate_molecule.getSequence();

		// extracts masses and abundances from isotope distribution of the candidate molecule
		masses_container candidate_masses = candidate_molecule_distribution.getMasses();
		abundances_container candidate_abundances = candidate_molecule_distribution.getAbundances();

		// normalizes candidate abundances if the size of the measured peaklist is less than 
		// the size of theoretical isotope distribution. This is always the case since our
		// theoretical distributions are limited to by default 10 peaks and measured peaklists contain
		// less than 10 peaks

		distribution_t::size_type size = min(peaklist_abundances.size(), candidate_abundances.size());
		
		if (size < candidate_abundances.size()) {
			// normalizes the isotope distribution abundances with respect to the number of elements in peaklist
			abundance_type sum = accumulate(candidate_abundances.begin(), 
							candidate_abundances.begin() + size, 
							0.0);
			if (fabs(sum - 1) > IsotopeDistribution::ABUNDANCES_SUM_ERROR) {
				abundance_type scale = 1/sum;
				transform(candidate_abundances.begin(),			// begin of source range
					candidate_abundances.begin() + size,		// end of source range
					candidate_abundances.begin(), 			// destination
					bind2nd(multiplies<abundance_type>(), scale));	// operation (*scale)
			}
				
		}

		// calculates a score
		score_type score = scorer.score(candidate_masses, candidate_abundances);

		// stores the sequence with non-normalized score
		nonnormalized_scores.push_back(make_pair(candidate_molecule, score));

		// accumulates scores
		accumulated_score += score;

	}

	for (nonnormalized_scores_container::const_iterator it = nonnormalized_scores.begin(); it != nonnormalized_scores.end(); ++it) {
		score_type normalized_score = it->second;
		if (accumulated_score > 0.0) {
			normalized_score /= accumulated_score;
		}
		// stores the sequence with the score
		scores.insert(make_pair(normalized_score, it->first));
	}

	// Now output to R ...
	if (scores.size() >0 ) {
	  rl = rlistScores(scores, INTEGER_VALUE(z));
	}
    } catch(std::exception& ex) {
      exceptionMesg = copyMessageToR(ex.what());
      error_return(exceptionMesg); 
    } catch(...) {
      exceptionMesg = copyMessageToR("unknown reason");
      error_return(exceptionMesg); 
    }
        
    return rl;

}

// }}}

RcppExport SEXP calculateScore(SEXP v_predictMasses, SEXP v_predictAbundances, SEXP v_measuredMasses, SEXP v_meausuredAbundances) {
//  {{{
	typedef DistributionProbabilityScorer scorer_type;
    	typedef scorer_type::score_type score_type;
    	typedef scorer_type::masses_container masses_container;
    	typedef scorer_type::abundances_container abundances_container;
    	typedef distribution_t::peaks_container peaks_container;
    	typedef distribution_t::mass_type mass_type;
    	typedef distribution_t::abundance_type abundance_type;



	RcppVector<double> masses = RcppVector<double>(v_predictMasses);
	RcppVector<double> abundances = RcppVector<double>(v_predictAbundances);

	// fills peaklist masses and abundances, 
	// since we cannot use masses and abundances - instances of RcppVector object - directly

	masses_container peaklist_masses;
	abundances_container peaklist_abundances;
	for (masses_container::size_type mi = 0; mi < masses.size() && mi < abundances.size(); ++mi)
	{
		peaklist_masses.push_back(masses(mi));
		peaklist_abundances.push_back(abundances(mi));
	}

	// initializes distribution probability scorer
	scorer_type scorer(peaklist_masses, peaklist_abundances);

	masses = RcppVector<double>(v_measuredMasses);
	abundances = RcppVector<double>(v_meausuredAbundances);
	//cout << "Mess:" << masses<<"\n";
	masses_container mess_masses;
	abundances_container mess_abundances;

	// normalizes abundances
	abundance_type abundances_sum = 0.0;
	for (peaks_container::size_type i = 0; i < abundances.size(); ++i) {
		abundances_sum += abundances(i);
	}
	for (peaks_container::size_type i = 0; i < abundances.size(); ++i) {
		abundances(i) /= abundances_sum;
	}	

	for (masses_container::size_type mi = 0; mi < masses.size() && mi < abundances.size(); ++mi)
	{
		mess_masses.push_back(masses(mi));
		mess_abundances.push_back(abundances(mi));
	}

	score_type score=scorer.score(mess_masses, mess_abundances);
	SEXP output;
	PROTECT(output = NEW_NUMERIC(1));
	//cout<< score;
	REAL(output)[0]=score;
	UNPROTECT(1);
 	return (output);
}
// }}}


RcppExport SEXP getMolecule(SEXP s_formula, SEXP l_alphabet, 
			    SEXP v_element_order, SEXP z, SEXP i_maxisotopes) {
  // {{{ 

  SEXP  rl=R_NilValue; // Use this when there is nothing to be returned.

   if( (s_formula==NULL) || !isString(s_formula) || length(s_formula) != 1)
         error("formula is not a single string");

  typedef DistributionProbabilityScorer scorer_type;
  typedef scorer_type::score_type score_type;
  typedef multimap<score_type, ComposedElement, greater<score_type> > scores_container;

  // Reset error state
  exceptionMesg = NULL;

  // initializes alphabet
  int maxisotopes = INTEGER_VALUE(i_maxisotopes);
  alphabet_t alphabet;
  vector<string> elements_order;

  if (l_alphabet == NULL || length(l_alphabet) < 1  ) {
    initializeCHNOPS(alphabet, maxisotopes); 
    // initializes order of atoms in which one would
    // like them to appear in the molecules sequence
    elements_order.push_back("C");
    elements_order.push_back("H");
    elements_order.push_back("N");
    elements_order.push_back("O");
    elements_order.push_back("P");
    elements_order.push_back("S");
  } else {
    initializeAlphabet(l_alphabet, alphabet, maxisotopes);

    int element_length = length(v_element_order);
    for (int i=0; i<element_length; i++) {
      elements_order.push_back(string(CHAR(STRING_ELT(v_element_order,i))));
    }
  }

  try {
    // initializes precision
    double precision = 1.0e-05;
    
    // initializes weights
    Weights weights(alphabet.getMasses(), precision);

    // initializes storage for scores
    scores_container scores;
    
    ComposedElement molecule( CHARACTER_VALUE(s_formula), alphabet);
    
    molecule.updateSequence(&elements_order);
    molecule.updateIsotopeDistribution();
    
    scores.insert(make_pair(1.0, molecule));
    rl = rlistScores(scores, INTEGER_VALUE(z));
  } catch(std::exception& ex) {
    exceptionMesg = copyMessageToR(ex.what());
    error(exceptionMesg); 
  } catch(...) {
    exceptionMesg = copyMessageToR("unknown reason");
    error_return(exceptionMesg); 
  }

  return rl;
}

// }}}

RcppExport SEXP addMolecules(SEXP s_formula1, SEXP s_formula2, SEXP l_alphabet, 
			     SEXP v_element_order, SEXP maxisotopes) {
  // {{{ 

  SEXP  rl=R_NilValue; // Use this when there is nothing to be returned.

   if( (s_formula1==NULL) || s_formula2==NULL 
       || !isString(s_formula1) || !isString(s_formula2) 
       || length(s_formula2) != 1) {
         error("formula is not a single string");
   }

  typedef DistributionProbabilityScorer scorer_type;
  typedef scorer_type::score_type score_type;
  typedef multimap<score_type, ComposedElement, greater<score_type> > scores_container;

  // initializes alphabet
  alphabet_t alphabet;
  vector<string> elements_order;

  if (l_alphabet == NULL || length(l_alphabet) < 1  ) {
    initializeCHNOPS(alphabet, 0); 
    // initializes order of atoms in which one would
    // like them to appear in the molecules sequence
    elements_order.push_back("C");
    elements_order.push_back("H");
    elements_order.push_back("N");
    elements_order.push_back("O");
    elements_order.push_back("P");
    elements_order.push_back("S");
  } else {
    initializeAlphabet(l_alphabet, alphabet, 0);

    int element_length = length(v_element_order);
    for (int i=0; i<element_length; i++) {
      elements_order.push_back(string(CHAR(STRING_ELT(v_element_order,i))));
    }
  }
//  cout << alphabet << endl;
  // initializes precision
  double precision = 1.0e-05;
  
  // initializes weights
  Weights weights(alphabet.getMasses(), precision);

  // initializes storage for scores
  scores_container scores;

  ComposedElement molecule( CHARACTER_VALUE(s_formula1), alphabet);
  ComposedElement molecule2( CHARACTER_VALUE(s_formula2), alphabet);

  molecule += molecule2;

  molecule.updateSequence(&elements_order);
  molecule.updateIsotopeDistribution();

  scores.insert(make_pair(1.0, molecule));
  rl = rlistScores(scores, 0);
  return rl;
}

// }}}

RcppExport SEXP subMolecules(SEXP s_formula1, SEXP s_formula2, SEXP l_alphabet, SEXP v_element_order) {
  // {{{ 

  SEXP  rl=R_NilValue; // Use this when there is nothing to be returned.

   if( (s_formula1==NULL) || s_formula2==NULL 
       || !isString(s_formula1) || !isString(s_formula2) 
       || length(s_formula2) != 1) {
         error("formula is not a single string");
   }

  typedef DistributionProbabilityScorer scorer_type;
  typedef scorer_type::score_type score_type;
  typedef multimap<score_type, ComposedElement, greater<score_type> > scores_container;

  // initializes alphabet
  alphabet_t alphabet;
  vector<string> elements_order;

  if (l_alphabet == NULL || length(l_alphabet) < 1  ) {
    initializeCHNOPS(alphabet, 0); 
    // initializes order of atoms in which one would
    // like them to appear in the molecules sequence
    elements_order.push_back("C");
    elements_order.push_back("H");
    elements_order.push_back("N");
    elements_order.push_back("O");
    elements_order.push_back("P");
    elements_order.push_back("S");
  } else {
    initializeAlphabet(l_alphabet, alphabet, 0);

    int element_length = length(v_element_order);
    for (int i=0; i<element_length; i++) {
      elements_order.push_back(string(CHAR(STRING_ELT(v_element_order,i))));
    }
  }
//  cout << alphabet << endl;
  // initializes precision
  double precision = 1.0e-05;
  
  // initializes weights
  Weights weights(alphabet.getMasses(), precision);

  // initializes storage for scores
  scores_container scores;

  ComposedElement molecule( CHARACTER_VALUE(s_formula1), alphabet);
  ComposedElement molecule2( CHARACTER_VALUE(s_formula2), alphabet);

  molecule -= molecule2;

  molecule.updateSequence(&elements_order);
  molecule.updateIsotopeDistribution();

  scores.insert(make_pair(1.0, molecule));
  rl = rlistScores(scores, 0);
  return rl;
}

// }}}

template <typename score_type>
SEXP  rlistScores(multimap<score_type, ComposedElement, greater<score_type> > scores, int z) {
  // {{{ 

    typedef DistributionProbabilityScorer scorer_type;
    typedef multimap<score_type, ComposedElement, greater<score_type> > scores_container;

	// Build result set to be returned as a list to R.
	vector<string> formula(scores.size());
	RcppVector<double> score(scores.size());
	RcppVector<double> exactmass(scores.size());
	RcppVector<int> charge(scores.size());

	// Chemical rules
	vector<string> parity(scores.size());
	vector<string> valid(scores.size());
	RcppVector<double> DBE(scores.size());

	SEXP isotopes = PROTECT(allocVector(VECSXP, scores.size()));

 	RcppResultSet rs;
	unsigned int i = 0;

	vector<string> colNames(2);
	colNames[0] = "mass";
	colNames[1] = "intensity";

	// outputs molecules & their scores.
	for (typename scores_container::const_iterator it = scores.begin(); 
				it != scores.end(); ++it) {
		score(i) = it->first;
      		formula[i] = it->second.getSequence();
      		exactmass(i) = it->second.getMass();
      		charge(i) = z;

		// Chemical rules 
		parity[i] = getParity(it->second, z);
		valid[i] = isValidMyNitrogenRule(it->second, z) ? "Valid" : "Invalid";

		DBE(i) = getDBE(it->second, z);

		IsotopeDistribution isodist = it->second.getIsotopeDistribution();
		
		IsotopeDistribution::masses_container masses = isodist.getMasses();
		IsotopeDistribution::abundances_container intensities = isodist.getAbundances();

		int ny = masses.size();
		SEXP tmp_isotopes = PROTECT(allocMatrix(REALSXP, 2, ny));

		for(int j = 0; j < ny; j++) {
		    REAL(tmp_isotopes)[0 + 2*j] = masses[j];
		    REAL(tmp_isotopes)[1 + 2*j] = intensities[j];
		}
		SET_VECTOR_ELT(isotopes, i, tmp_isotopes);

		UNPROTECT(1);

		i++;
	}

	rs.add("formula", formula);
	rs.add("score", score);
	rs.add("exactmass", exactmass);
	rs.add("charge", z);

	rs.add("parity", parity);
	rs.add("valid", valid);
	rs.add("DBE", DBE);

	rs.add("isotopes", isotopes, true);

	// Get the list to be returned to R.
	SEXP rl = rs.getReturnList();

	if(exceptionMesg != NULL) {
	  error(exceptionMesg);
	}
	return rl;

	// }}}
}

//
// Initialisation of Standard Element Alphabet 
//
void initializeCHNOPS(alphabet_t& chnops, const int maxisotopes) {
  // {{{ 

	typedef distribution_t::peaks_container peaks_container;
	typedef distribution_t::nominal_mass_type nominal_mass_type;
	typedef alphabet_t::element_type element_type;
	typedef alphabet_t::container elements_type;

	distribution_t::SIZE = maxisotopes;
	distribution_t::ABUNDANCES_SUM_ERROR = 0.00001;

// Hydrogen
	nominal_mass_type massH = 1;
	peaks_container peaksH;
	peaksH.push_back(peaks_container::value_type(0.007825, 0.99985));
	peaksH.push_back(peaks_container::value_type(0.014102, 0.00015));

	distribution_t distributionH(peaksH, massH);

// Oxygen
	nominal_mass_type massO = 16;
	peaks_container peaksO;
	peaksO.push_back(peaks_container::value_type(-0.005085, 0.99762));
	peaksO.push_back(peaks_container::value_type(-0.000868, 0.00038));
	peaksO.push_back(peaks_container::value_type(-0.000839, 0.002));

	distribution_t distributionO(peaksO, massO);

// Carbonate
	nominal_mass_type massC = 12;
	peaks_container peaksC;
	peaksC.push_back(peaks_container::value_type(0.0, 0.9889));
	peaksC.push_back(peaks_container::value_type(0.003355, 0.0111));

	distribution_t distributionC(peaksC, massC);

// Nitrogen
	nominal_mass_type massN = 14;
	peaks_container peaksN;
	peaksN.push_back(peaks_container::value_type(0.003074, 0.99634));
	peaksN.push_back(peaks_container::value_type(0.000109, 0.00366));

	distribution_t distributionN(peaksN, massN);

// Sulfur
	nominal_mass_type massS = 32;
	peaks_container peaksS;
	peaksS.push_back(peaks_container::value_type(-0.027929, 0.9502));
	peaksS.push_back(peaks_container::value_type(-0.028541, 0.0075));
	peaksS.push_back(peaks_container::value_type(-0.032133, 0.0421));
	peaksS.push_back(peaks_container::value_type());
	peaksS.push_back(peaks_container::value_type(-0.032919, 0.0002));

	distribution_t distributionS(peaksS, massS);

// Phosphor
	nominal_mass_type massP = 31;
	peaks_container peaksP;
	peaksP.push_back(peaks_container::value_type(-0.026238, 1.0));

	distribution_t distributionP(peaksP, massP);

	element_type H("H", distributionH);
	element_type C("C", distributionC);
	element_type N("N", distributionN);
	element_type O("O", distributionO);
	element_type P("P", distributionP);
	element_type S("S", distributionS);

	chnops.push_back(H);
	chnops.push_back(C);
	chnops.push_back(N);
	chnops.push_back(O);
	chnops.push_back(P);
	chnops.push_back(S);

	// }}}
}

//
// Initialisation of User-defined Alphabet 
//

/* get the list element named str, or return NULL */
/* http://cran.r-project.org/doc/manuals/R-exts.html#Handling-lists */

SEXP getListElement(SEXP list, char *str)
  // {{{ 

{
  SEXP elmt = R_NilValue, names = getAttrib(list, R_NamesSymbol);
  int i;
  
  for (i = 0; i < length(list); i++)
    if(strcmp(CHAR(STRING_ELT(names, i)), str) == 0) {
      elmt = VECTOR_ELT(list, i);
      break;
    }
  return elmt;
}

// }}}

void initializeAlphabet(const SEXP l_alphabet, 
			alphabet_t &alphabet,
			const int maxisotopes) {
  // {{{ 

  typedef distribution_t::peaks_container peaks_container;
  typedef distribution_t::nominal_mass_type nominal_mass_type;
  typedef alphabet_t::element_type element_type;
  typedef alphabet_t::container elements_type;

  distribution_t::SIZE = maxisotopes;
  distribution_t::ABUNDANCES_SUM_ERROR = 0.0001;
       
  for (int i=0; i < length(l_alphabet); i++) {
    SEXP l = VECTOR_ELT(l_alphabet,i);
	  
    const char *symbol = STRING_VALUE(getListElement(l, "name"));

    nominal_mass_type nominalmass = (nominal_mass_type) REAL(getListElement(l, "mass"))[0];
	  	  
    SEXP isotope = getListElement(l, "isotope");	

    int numisotopes = length(getListElement(isotope, "mass"));
    double *mass = REAL(getListElement(isotope, "mass"));	
    double *abundance = REAL(getListElement(isotope, "abundance"));	

    peaks_container *peaks = new peaks_container();
    for (int j=0; j<numisotopes; j++) {
      peaks->push_back(peaks_container::value_type(mass[j], abundance[j]));
    }
    distribution_t *distribution = new distribution_t(*peaks, nominalmass);

    element_type element(symbol, *distribution);	
    alphabet.push_back(element);
  }

  // }}}

}

extern "C" {

  void R_init_disop(DllInfo *info)
  {
    /* Register routines, allocate resources.
     * We call most functions with .Call 
     */
    R_CallMethodDef callMethods[]  = {
      {"getMolecule", (void* (*)())&getMolecule, 4},
      {"addMolecules", (void* (*)())&addMolecules, 4},
      {"subMolecules", (void* (*)())&subMolecules, 4},
      {"decomposeIsotopes", (void* (*)())&decomposeIsotopes, 7},
      {"calculateScore", (void* (*)())&calculateScore, 7},
      {NULL, NULL, 0}
    };
    
    R_registerRoutines(info, NULL, callMethods, NULL, NULL);
  }
  
  void
  R_unload_disop(DllInfo *info)
  {
    /* Release resources. */
  }

}
