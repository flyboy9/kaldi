// lm/kaldi-lmtable.cc
//
// Copyright 2009-2011 Gilles Boulianne.
//
// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//  http://www.apache.org/licenses/LICENSE-2.0

// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.
/**
 * @file kaldi-lmtable.cc
 * @brief Implementation of internal representation for language model.
 *
 * See kaldi-lmtable.h for more details.
 */

#include "lm/kaldi-lmtable.h"
#include "base/kaldi-common.h"
#include <sstream>

namespace kaldi {

// typedef fst::StdArc::StateId StateId;

// newly_added will be updated
LmFstConverter::StateId LmFstConverter::AddStateFromSymb(
    const std::vector<string> &ngramString,
    int kstart, int kend,
    fst::StdVectorFst *pfst,
    bool &newly_added) {
  fst::StdArc::StateId sid;
  std::string separator;
  separator.resize(1);
  separator[0] = '\0';
  
  std::string hist;
  if (kstart == 0) {
    hist.append(separator);
  } else {
    for (int k = kstart; k >= kend; k--) {
      hist.append(ngramString[k]);
      hist.append(separator);
    }
  }

  newly_added = false;
  sid = FindState(hist);
  if (sid < 0) {
    sid = pfst->AddState();
    hist_state_[hist] = sid; 
    newly_added = true;
    //cerr << "Created state " << sid << " for " << hist << endl;
  } else {
    //cerr << "State symbol " << hist << " already exists" << endl;
  }

  return sid;
}

void LmFstConverter::ConnectUnusedStates(fst::StdVectorFst *pfst) {

  // go through all states with a recorded backoff destination 
  // and find out any that has no output arcs and is not final
  unsigned int connected = 0;
  // cerr << "ConnectUnusedStates has recorded "<<backoff_state_.size()<<" states.\n";

  for (BackoffStateMap::iterator bkit = backoff_state_.begin(); bkit != backoff_state_.end(); ++bkit) {
    // add an output arc to its backoff destination recorded in backoff_
    fst::StdArc::StateId src = bkit->first, dst = bkit->second;
    if (pfst->NumArcs(src)==0 && !IsFinal(pfst, src)) {
      // cerr << "ConnectUnusedStates: adding arc from "<<src<<" to "<<dst<<endl;
      // epsilon arc with no cost
      pfst->AddArc(src,
                   fst::StdArc(0, 0, fst::StdArc::Weight::One(), dst));
      connected++;
    }
  }
  cerr << "Connected " << connected << " states without outgoing arcs." << endl;
}

void LmFstConverter::AddArcsForNgramProb(
    int ngram_order, int max_ngram_order,
    float logProb,
    float logBow,
    std::vector<string> &ngram,
    fst::StdVectorFst *fst,
    const string startSent,
    const string endSent) {
  fst::StdArc::StateId src, dst, dbo;
  std::string curwrd = ngram[1];
  if (curwrd == "<eps>") {
    KALDI_ERR << "The word <eps> is not allowed as a word in an ARPA LM.";
  }
  int64 ilab, olab;
  LmWeight prob = ConvertArpaLogProbToWeight(logProb);
  LmWeight bow  = ConvertArpaLogProbToWeight(logBow);
  bool newSrc, newDbo, newDst = false;

  if (ngram_order >= 2) {
    // General case works from N down to 2-grams
    src = AddStateFromSymb(ngram, ngram_order, 2, fst, newSrc);
    if (ngram_order != max_ngram_order) {
      // add all intermediate levels from 2 to current
      // last ones will be current backoff source and destination
      for (int i = 2; i <= ngram_order; i++) {
        dst = AddStateFromSymb(ngram, i,   1, fst, newDst);
        dbo = AddStateFromSymb(ngram, i-1, 1, fst, newDbo);
        backoff_state_[dst] = dbo;
      }
    } else {
      // add all intermediate levels from 2 to current
      // last ones will be current backoff source and destination
      for (int i = 2; i <= ngram_order; i++) {
        dst = AddStateFromSymb(ngram, i-1, 1, fst, newDst);
        dbo = AddStateFromSymb(ngram, i-2, 1, fst, newDbo);
        backoff_state_[dst] = dbo;
      }
    }
  } else {
    // special case for 1-grams: start from 0-gram
    if (curwrd.compare(startSent) != 0) {
      src = AddStateFromSymb(ngram, 0, 1, fst, newSrc);
    } else {
      // extra special case if in addition we are at beginning of sentence
      // starts from initial state and has no cost
      src = fst->Start();
      prob = fst::StdArc::Weight::One();
    }
    dst = AddStateFromSymb(ngram, 1, 1, fst, newDst);
    dbo = AddStateFromSymb(ngram, 0, 1, fst, newDbo);
    backoff_state_[dst] = dbo;
  }

  // state is final if last word is end of sentence
  if (curwrd.compare(endSent) == 0) {
    fst->SetFinal(dst, fst::StdArc::Weight::One());
  }
  // add labels to symbol tables
  ilab = fst->MutableInputSymbols()->AddSymbol(curwrd);
  olab = fst->MutableOutputSymbols()->AddSymbol(curwrd);

  // add arc with weight "prob" between source and destination states
  // cerr << "n-gram prob, fstAddArc: src "<< src << " dst " << dst;
  // cerr << " lab " << ilab << endl;
  fst->AddArc(src, fst::StdArc(ilab, olab, prob, dst));

  // add backoffs to any newly created destination state
  // but only if non-final
  if (!IsFinal(fst, dst) && newDst && dbo != dst) {
    ilab = olab = 0;
    // cerr << "backoff, fstAddArc: src "<< src << " dst " << dst;
    // cerr << " lab " << ilab << endl;
    fst->AddArc(dst, fst::StdArc(ilab, olab, bow, dbo));
  }
}

#ifndef HAVE_IRSTLM

bool LmTable::ReadFstFromLmFile(std::istream &istrm,
                                fst::StdVectorFst *fst,
                                bool useNaturalOpt,
                                const string startSent,
                                const string endSent) {
#ifdef KALDI_PARANOID
  KALDI_ASSERT(fst);
  KALDI_ASSERT(fst->InputSymbols() && fst->OutputSymbols());
#endif

  conv_->UseNaturalLog(useNaturalOpt);

  // do not use state symbol table for word histories anymore
  string inpline;
  size_t pos1, pos2;
  int ngram_order, max_ngram_order = 0;

  // process \data\ section

  while (getline(istrm, inpline) && !istrm.eof()) {
    std::istringstream ss(inpline);
    std::string token;
    ss >> token >> std::ws;
    if (token == "\\data\\" && ss.eof()) break;
  }
  if (istrm.eof()) {
    KALDI_ERR << "\\data\\ token not found in arpa file.";
  }

  std::vector<int> orders; 
  while (getline(istrm, inpline) && !istrm.eof()) {
    // break out of loop if another section is found
    if (inpline.find("-grams:") != string::npos) break;
    if (inpline.find("\\end\\") != string::npos) break;

    // look for valid "ngram N = M" lines
    pos1 = inpline.find("ngram");
    pos2 = inpline.find("=");
    if (pos1 == string::npos ||  pos2 == string::npos || pos2 <= pos1) {
      continue;  // not valid, continue looking
    }
    // found valid line
    ngram_order = atoi(inpline.substr(pos1+5, pos2-(pos1+5)).c_str());
    orders.push_back(ngram_order);
    if (ngram_order > max_ngram_order) {
      max_ngram_order = ngram_order;
    }
  }
  if (max_ngram_order == 0) {
    // reached end of loop without having found any n-gram
    KALDI_ERR << "No ngrams found in specified file";
  }

  for (int32 i = 0; i < orders.size(); i++) {
    if (orders[i] != i+1)
      KALDI_ERR << (i + 1) <<"-grams not specified in arpa file";
  }
 
  // process "\N-grams:" sections, we may have already read a "\N-grams:" line
  // if so, process it, otherwise get another line
  while (inpline.find("-grams:") != string::npos
         || (getline(istrm, inpline) && !istrm.eof()) ) {
    // look for a valid "\N-grams:" section
    pos1 = inpline.find("\\");
    pos2 = inpline.find("-grams:");
    if (pos1 == string::npos || pos2 == string::npos || pos2 <= pos1) {
      continue;  // not valid line, continue looking for one
    }
    // found, set current level
    ngram_order = atoi(inpline.substr(pos1+1, pos2-(pos1+1)).c_str());
    if (orders[0] != ngram_order)
      KALDI_ERR << ngram_order << "-grams not specified in arpa header, or "
                << "statistics of "<< orders[0] << "-grams not provided ? "
                << "Check your arpa lm file.";
    else {
      cerr << "Processing " << ngram_order << "-grams" << endl;
      orders.erase(orders.begin());
    }

    // process individual n-grams
    while (getline(istrm, inpline) && !istrm.eof()) {
      // break out of inner loop if another section is found
      if (!inpline.empty() && inpline[0] == '\\') {
        if (inpline.find("-grams:") != string::npos) break;
        if (inpline.find("\\end\\") != string::npos) break;
      }
      // parse ngram line: first field = prob, other fields = words,
      // last field = backoff (optional)
      std::vector<string> ngramString;
      float prob, bow;

      // eat up space.
      const char *cur_cstr = inpline.c_str();
      while (*cur_cstr && isspace(*cur_cstr))
        cur_cstr++;

      if (*cur_cstr == '\0') // Ignore empty lines.
        continue;
      char *next_cstr;
      // found, parse probability from first field
      prob = KALDI_STRTOF(cur_cstr, &next_cstr);
      if (prob != prob || prob - prob != 0) {
        KALDI_ERR << "nan or inf detected in LM file [parsing " << (ngram_order)
            << "-grams]: " << inpline;
      }
      if (next_cstr == cur_cstr)
        KALDI_ERR << "Bad line in LM file [parsing "<<(ngram_order)<<"-grams]: "<<inpline;
      cur_cstr = next_cstr;
      while (*cur_cstr && isspace(*cur_cstr))
        cur_cstr++;

      // element 0 will be empty, element 1 will be the current word,
      // element 2 will be the immediately preceding word, and so on.
      // Apparently an IRSTLM convention.
      ngramString.resize(ngram_order + 1);
      
      bool illegal_bos_or_eos = false;
      for (int i = 0; i < ngram_order; i++) {
        if (*cur_cstr == '\0')
          KALDI_ERR << "Bad line in LM file [parsing "<<(ngram_order)<<"-grams]: "<<inpline;

        const char *end_cstr = strpbrk(cur_cstr, " \t\r");
        std::string this_word;
        if (end_cstr == NULL) {
          this_word = std::string(cur_cstr);
          cur_cstr += strlen(cur_cstr);
        } else {
          this_word = std::string(cur_cstr, end_cstr-cur_cstr);
          cur_cstr = end_cstr;
          while (*cur_cstr && isspace(*cur_cstr))
            cur_cstr++;
        }

        // Checks if <s> only appears at the beginning of the ngram, and if </s>
        // only appears at the end of the ngram.
        if ((ngram_order > 1 && i != 0 && this_word == "<s>") ||
            (ngram_order > 1 && i != ngram_order - 1 && this_word == "</s>")) {
          illegal_bos_or_eos = true;
          break;
        }

        ngramString[ngram_order - i].swap(this_word);
      }
      if (illegal_bos_or_eos) {
        KALDI_WARN << "<s> is not at the beginning of the n-gram, or </s> is "
            << "not at the end of the n-gram, skipping it: " << inpline;
        continue;
      }

      bow = 0;
      if (ngram_order < max_ngram_order) {
        // try converting anything left in the line to a backoff weight
        if (*cur_cstr != '\0') {
          char *end_cstr;
          bow = KALDI_STRTOF(cur_cstr, &end_cstr);
          if (bow != bow || bow - bow != 0) {
            KALDI_ERR << "nan or inf detected in LM file [parsing " << (ngram_order)
                << "-grams]: " << inpline;
          }
          if (end_cstr != cur_cstr) {  // got something.
            while (*end_cstr != '\0' && isspace(*end_cstr))
              end_cstr++;
            if (*end_cstr != '\0')
              KALDI_ERR << "Junk " << (end_cstr) << " at end of line [parsing "
                        << (ngram_order) << "-grams]" << inpline;
          } else {
            KALDI_ERR << "Junk " << (cur_cstr) << " at end of line [parsing "
                      << (ngram_order) << "-grams]" << inpline;
          }
        }
      }
      conv_->AddArcsForNgramProb(ngram_order, max_ngram_order, prob, bow,
                                 ngramString, fst,
                                 startSent, endSent);
    }  // end of loop on individual n-gram lines
  }
  if (orders.size() > 0)
    KALDI_ERR << orders[0] << "-grams specified in arpa header " 
              << "but no statistics provided to build FST";

  conv_->ConnectUnusedStates(fst);

  // not used anymore: delete pStateSymbs;

  // input and output symbol tables will be deleted by ~fst()
  return true;
}

#else

// #ifdef HAVE_IRSTLM implementation

bool LmTable::ReadFstFromLmFile(std::istream &istrm,
                                fst::StdVectorFst *fst,
                                bool useNaturalOpt,
                                const string startSent,
                                const string endSent) {
  load(istrm, "input name?", "output name?", 0, NONE);
  ngram ng(this->getDict(), 0);

  conv_->UseNaturalLog(useNaturalOpt);
  DumpStart(ng, fst, startSent, endSent);

  // should do some check before returning true
  return true;
}

// run through all nodes in table (as in dumplm)
void LmTable::DumpStart(ngram ng,
                        fst::StdVectorFst *fst,
                        const string startSent,
                        const string endSent) {
#ifdef KALDI_PARANOID
  KALDI_ASSERT(fst);
  KALDI_ASSERT(fst->InputSymbols() && fst->OutputSymbols());
#endif
  // we need a state symbol table while traversing word contexts
  fst::SymbolTable *pStateSymbs = new fst::SymbolTable("kaldi-lm-state");

  // dump level by level
  for (int l = 1; l <= max_ngram_order; l++) {
    ng.size = 0;
    cerr << "Processing " << l << "-grams" << endl;
    DumpContinue(ng, 1, l, 0, cursize[1],
                 fst, pStateSymbs, startSent, endSent);
  }

  delete pStateSymbs;
  // input and output symbol tables will be deleted by ~fst()
}

// run through given levels and positions in table
void LmTable::DumpContinue(ngram ng, int ngram_order, int elev,
                           table_entry_pos_t ipos, table_entry_pos_t epos,
                           fst::StdVectorFst *fst,
                           fst::SymbolTable *pStateSymbs,
                           const string startSent, const string endSent) {
  LMT_TYPE ndt = tbltype[ngram_order];
  ngram ing(ng.dict);
  int ndsz = nodesize(ndt);

#ifdef KALDI_PARANOID
  KALDI_ASSERT(ng.size == ngram_order - 1);
  KALDI_ASSERT(ipos >= 0 && epos <= cursize[ngram_order] && ipos < epos);
  KALDI_ASSERT(pStateSymbs);
#endif

  ng.pushc(0);

  for (table_entry_pos_t i = ipos; i < epos; i++) {
    *ng.wordp(1) = word(table[ngram_order] + (table_pos_t)i * ndsz);
    float ipr = prob(table[ngram_order] + (table_pos_t)i * ndsz, ndt);
    // int ipr = prob(table[ngram_order] + i * ndsz, ndt);
    // skip pruned n-grams
    if (isPruned && ipr == NOPROB) continue;

    if (ngram_order < elev) {
      // get first and last successor position
      table_entry_pos_t isucc = (i > 0 ? bound(table[ngram_order] +
                                               (table_pos_t) (i-1) * ndsz,
                                               ndt) : 0);
      table_entry_pos_t esucc = bound(table[ngram_order] +
                                      (table_pos_t) i * ndsz, ndt);
      if (isucc < esucc)  // there are successors!
        DumpContinue(ng, ngram_order+1, elev, isucc, esucc,
                     fst, pStateSymbs, startSent, endSent);
      // else
      // cerr << "no successors for " << ng << "\n";
    } else {
      // cerr << i << " ";  // this was just to count printed n-grams
      // cerr << ipr <<"\t";
      // cerr << (isQtable?ipr:*(float *)&ipr) <<"\t";

      // if table is inverted then revert n-gram
      if (isInverted && ng.size > 1) {
        ing.invert(ng);
        ng = ing;
      }

      // cerr << "ngram_order " << ngram_order << " ngsize " << ng.size << endl;

      // for FST creation: vector of words strings
      std::vector<string> ngramString;
      for (int k = ng.size; k >= 1; k--) {
        // words are inserted so position 1 is most recent word,
        // and position N oldest word (IRSTLM convention)
        ngramString.insert(ngramString.begin(),
                           this->getDict()->decode(*ng.wordp(k)));
      }
      // reserve index 0 so that words go from 1, .., ng.size-1
      ngramString.insert(ngramString.begin(), "");
      float ibo = 0;
      if (ngram_order < max_ngram_order) {
        // Backoff
        ibo = bow(table[ngram_order]+ (table_pos_t)i * ndsz, ndt);
        // if (isQtable) cerr << "\t" << ibo;
        // else if (ibo != 0.0) cerr << "\t" << ibo;
      }
      conv_->AddArcsForNgramProb(ngram_order, max_ngram_order, ipr, ibo,
                                 ngramString, fst, pStateSymbs,
                                 startSent, endSent);
    }
  }
}

#endif

}  // end namespace kaldi

