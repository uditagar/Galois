/** Support functions -*- C++ -*-
 * @file
 * @section License
 *
 * This file is part of Galois.  Galois is a framework to exploit
 * amorphous data-parallelism in irregular programs.
 *
 * Galois is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Galois is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Galois.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * @section Copyright
 *
 * Copyright (C) 2015, The University of Texas at Austin. All rights
 * reserved.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */
#include "Galois/Statistic.h"
#include "Galois/gdeque.h"
#include "Galois/Substrate/PerThreadStorage.h"
#include "Galois/Runtime/Support.h"
#include "Galois/Substrate/gio.h"
#include "Galois/Substrate/PaddedLock.h"
#include "Galois/Substrate/StaticInstance.h"
#include "Galois/Runtime/Mem.h"

#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <vector>

namespace Galois {
namespace Runtime {
extern unsigned activeThreads;
} } //end namespaces

using namespace Galois;
using namespace Galois::Runtime;

namespace {

class StatManager {

  template<typename Ty>
  using StringPair = std::pair<const std::string*, Ty>;

  //////////////////////////////////////
  //Symbol Table
  //////////////////////////////////////
  std::set<std::string> symbols;
  const std::string* getSymbol(const std::string& str) const {
    auto ii = symbols.find(str);
    if (ii == symbols.cend())
      return nullptr;
    return &*ii;
  }
  const std::string* getOrInsertSymbol(const std::string& str) {
    auto ii = symbols.insert(str);
    return &*ii.first;
  }

  //////////////////////////////////////
  //Loop instance counter
  //////////////////////////////////////
  std::vector<StringPair<unsigned> > loopInstances;

  unsigned getInstanceNum(const std::string& str) const {
    auto s = getSymbol(str);
    if (!s)
      return 0;
    auto ii = std::lower_bound(loopInstances.begin(), loopInstances.end(), s, [] (const StringPair<unsigned>& s1, const std::string* s2) { return s1.first < s2; } );
    if (ii == loopInstances.end() || s != ii->first)
      return 0;
    return ii->second;
  }

  void addInstanceNum(const std::string& str) {
    auto s = getOrInsertSymbol(str);
    auto ii = std::lower_bound(loopInstances.begin(), loopInstances.end(), s, [] (const StringPair<unsigned>& s1, const std::string* s2) { return s1.first < s2; } );
    if (ii == loopInstances.end() || s != ii->first) {
      loopInstances.emplace_back(s, 0);
      std::sort(loopInstances.begin(), loopInstances.end(), [] (const StringPair<unsigned>& s1, const StringPair<unsigned>& s2) { return s1.first < s2.first; } );
    } else {
      ++ii->second;
    }
  }

  //////////////////////////////////////
  //Stat list
  //////////////////////////////////////
  struct RecordTy {
    const std::string* loop;
    const std::string* category;
    unsigned instance;
    char mode; // 0 - int, 1 - double, 2 - string
    union {
      size_t valueInt;
      double valueDouble;
      std::string valueStr;
    };
    RecordTy(const std::string* loop, const std::string* category, unsigned instance, size_t value) :loop(loop), category(category),instance(instance), mode(0), valueInt(value) {}
    RecordTy(const std::string* loop, const std::string* category, unsigned instance, double value) :loop(loop), category(category),instance(instance), mode(1), valueDouble(value) {}
    RecordTy(const std::string* loop, const std::string* category, unsigned instance, const std::string& value) :loop(loop), category(category),instance(instance), mode(2), valueStr(value) {}

    ~RecordTy() {
      using string_type = std::string;
      if (mode == 2)
        valueStr.~string_type();
    }

    RecordTy(const RecordTy& r) : loop(r.loop), category(r.category), instance(r.instance), mode(r.mode) {
      switch(mode) {
      case 0: valueInt    = r.valueInt;    break;
      case 1: valueDouble = r.valueDouble; break;
      case 2: valueStr    = r.valueStr;    break;
      }
    }      

    void print(std::ostream& out) const {
      switch(mode) {
      case 0: out << valueInt;    break;
      case 1: out << valueDouble; break;
      case 2: out << valueStr;    break;
      }
    }
  };

  struct RecordList {
    Substrate::SimpleLock lock;
    gdeque<RecordTy> stats;

    template<typename T>
    void insertStat(const std::string* loop, const std::string* category, unsigned instance, const T& val) {
      MAKE_LOCK_GUARD(lock);
      stats.push_back(RecordTy(loop, category, instance, val));
    }
  };    


  Galois::Substrate::PerThreadStorage<RecordList> Stats;
  
 
  //////////////////////////////////////

public:
  StatManager() {}

  void addToStat(const std::string& loop, const std::string& category, size_t value) {
    Stats.getLocal()->insertStat(getOrInsertSymbol(loop), getOrInsertSymbol(category), getInstanceNum(loop), value);
  }
  void addToStat(const std::string& loop, const std::string& category, double value) {
    Stats.getLocal()->insertStat(getOrInsertSymbol(loop), getOrInsertSymbol(category), getInstanceNum(loop), value);
  }
  void addToStat(const std::string& loop, const std::string& category, const std::string& value) {
    Stats.getLocal()->insertStat(getOrInsertSymbol(loop), getOrInsertSymbol(category), getInstanceNum(loop), value);
  }

  void addToStat(Galois::Statistic* value) {
    for (unsigned x = 0; x < Galois::Runtime::activeThreads; ++x)
      Stats.getRemote(x)->insertStat(getOrInsertSymbol(value->getLoopname()), getOrInsertSymbol(value->getStatname()), 0, value->getValue(x));
  }

  void addPageAllocToStat(const std::string& loop, const std::string& category) {
    for (unsigned x = 0; x < Galois::Runtime::activeThreads; ++x)
      Stats.getRemote(x)->insertStat(getOrInsertSymbol(loop), getOrInsertSymbol(category), getInstanceNum(loop), static_cast<unsigned long>(numPagePoolAllocForThread(x)));
  }

  void addNumaAllocToStat(const std::string& loop, const std::string& category) {
    int nodes = Galois::Substrate::ThreadPool::getThreadPool().getMaxNumaNodes();
    for (int x = 0; x < nodes; ++x) {
      //auto rStat = Stats.getRemote(x);
      //std::lock_guard<Substrate::SimpleLock> lg(rStat->first);
      //      rStat->second.emplace_back(loop, category, numNumaAllocForNode(x));
    }
  }

  //assumne called serially
  void printStatsForR(std::ostream& out, bool json) {
    if (json)
      out << "[\n";
      else
        out << "LOOP,INSTANCE,CATEGORY,THREAD,VAL\n";
    for (unsigned x = 0; x < Stats.size(); ++x) {
      auto rStat = Stats.getRemote(x);
      MAKE_LOCK_GUARD(rStat->lock);
      for (auto& r : rStat->stats) {
        if (json)
          out << "{ \"LOOP\" : " << *r.loop << " , \"INSTANCE\" : " << r.instance << " , \"CATEGORY\" : " << *r.category << " , \"THREAD\" : " << x << " , \"VALUE\" : ";
        else
          out << *r.loop << "," << r.instance << "," << *r.category << "," << x << ",";
        r.print(out);
        out << (json ? "}\n" : "\n");
      }
    }
    if (json)
      out << "]\n";
  }

  //Assume called serially
  //still assumes int values
  void printStats(std::ostream& out) {
    std::map<std::tuple<const std::string*, unsigned, const std::string*>, std::vector<size_t> > LKs;
    
    unsigned maxThreadID = 0;
    //Find all loops and keys
    for (unsigned x = 0; x < Stats.size(); ++x) {
      auto rStat = Stats.getRemote(x);
      std::lock_guard<Substrate::SimpleLock> lg(rStat->lock);
      for (auto& r : rStat->stats) {
        maxThreadID = x;
	auto& v = LKs[std::make_tuple(r.loop, r.instance, r.category)];
        if (v.size() <= x)
          v.resize(x+1);
        v[x] += r.valueInt;
      }
    }
    //print header
    out << "STATTYPE,LOOP,INSTANCE,CATEGORY,n,sum";
    for (unsigned x = 0; x <= maxThreadID; ++x)
      out << ",T" << x;
    out << "\n";
    //print all values
    for (auto ii = LKs.begin(), ee = LKs.end(); ii != ee; ++ii) {
      std::vector<unsigned long>& Values = ii->second;
      out << "STAT,"
          << std::get<0>(ii->first)->c_str() << ","
          << std::get<1>(ii->first) << ","
          << std::get<2>(ii->first)->c_str() << ","
          << maxThreadID + 1 <<  ","
          << std::accumulate(Values.begin(), Values.end(), static_cast<unsigned long>(0));
      for (unsigned x = 0; x <= maxThreadID; ++x)
        out << "," <<  (x < Values.size() ? Values.at(x) : 0);
      out << "\n";
    }
  }

  void beginLoopInstance(const std::string& str) {
    addInstanceNum(str);
  }

};

static Substrate::StaticInstance<StatManager> SM;

}

void Galois::Runtime::reportLoopInstance(const char* loopname) {
  SM.get()->beginLoopInstance(std::string(loopname ? loopname : "(NULL)"));
}

void Galois::Runtime::reportStat(const char* loopname, const char* category, unsigned long value) {
  SM.get()->addToStat(std::string(loopname ? loopname : "(NULL)"), 
		      std::string(category ? category : "(NULL)"),
		      value);
}

void Galois::Runtime::reportStat(const std::string& loopname, const std::string& category, unsigned long value) {
  SM.get()->addToStat(loopname, category, value);
}

void Galois::Runtime::reportStat(Galois::Statistic* value) {
  SM.get()->addToStat(value);
}

void Galois::Runtime::reportStatGlobal(const std::string&, const std::string&) {
}
void Galois::Runtime::reportStatGlobal(const std::string&, unsigned long) {
}

#include <iostream>

void Galois::Runtime::printStats() {
  //  SM.get()->printStats(std::cout);
  SM.get()->printStatsForR(std::cout, false);
  //  SM.get()->printStatsForR(std::cout, true);
}

void Galois::Runtime::reportPageAlloc(const char* category) {
  SM.get()->addPageAllocToStat(std::string("(NULL)"), std::string(category ? category : "(NULL)"));
}

void Galois::Runtime::reportNumaAlloc(const char* category) {
  SM.get()->addNumaAllocToStat(std::string("(NULL)"), std::string(category ? category : "(NULL)"));
}