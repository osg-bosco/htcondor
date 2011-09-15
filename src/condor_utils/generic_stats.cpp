 /***************************************************************
 *
 * Copyright (C) 1990-2011, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_classad.h"
#include <map>
#include "timed_queue.h"
#include "generic_stats.h"
#include "classad_helpers.h" // for canStringForUseAsAttr
#include "string_list.h"     // for StringList

// use these to help initialize static const arrays arrays of 
//
#ifndef FIELDOFF
 #ifdef WIN32
  #define FIELDOFF(st,fld) FIELD_OFFSET(st, fld)
 #else
  //#define FIELDOFF(st,fld) ((int)(size_t)&(((st *)0)->fld))
  #define FIELDOFF(st,fld) offsetof(st,fld)
 #endif
 #define FIELDSIZ(st,fld) ((int)(sizeof(((st *)0)->fld)))
#endif
#define GS_FIELD(st,fld) (((st *)0)->fld)

// specialization of PublishDebug to dump the internal data storage
// of stats_entry_recent. 
//
template <class T>
void stats_entry_recent<T>::PublishDebug(ClassAd & ad, const char * pattr, int flags) const {
   MyString str;
      
   str += this->value;
   str += " ";
   str += this->recent;
   str.sprintf_cat(" {h:%d c:%d m:%d a:%d}", 
                   this->buf.ixHead, this->buf.cItems, this->buf.cMax, this->buf.cAlloc);
   if (this->buf.pbuf) {
      for (int ix = 0; ix < this->buf.cAlloc; ++ix) {
         str += !ix ? "[" : (ix == this->buf.cMax ? "|" : ",");
         str += this->buf.pbuf[ix];
         }
      str += "]";
      }

   MyString attr(pattr);
   if (flags & this->PubDecorateAttr)
      attr += "Debug";

   ad.Assign(pattr, str);
}

template <>
void stats_entry_recent<int64_t>::PublishDebug(ClassAd & ad, const char * pattr, int flags) const {
   MyString str;
   str += (long)this->value;
   str += " ";
   str += (long)this->recent;
   str.sprintf_cat(" {h:%d c:%d m:%d a:%d}", 
                   this->buf.ixHead, this->buf.cItems, this->buf.cMax, this->buf.cAlloc);
   if (this->buf.pbuf) {
      for (int ix = 0; ix < this->buf.cAlloc; ++ix) {
         str += !ix ? "[" : (ix == this->buf.cMax ? "|" : ",");
         str += (long)this->buf.pbuf[ix];
         }
      str += "]";
      }

   MyString attr(pattr);
   if (flags & this->PubDecorateAttr)
      attr += "Debug";

   ad.Assign(pattr, str);
}

template <>
void stats_entry_recent<double>::PublishDebug(ClassAd & ad, const char * pattr, int flags) const {
   MyString str;
   str.sprintf_cat("%g %g", this->value, this->recent);
   str.sprintf_cat(" {h:%d c:%d m:%d a:%d}", 
                   this->buf.ixHead, this->buf.cItems, this->buf.cMax, this->buf.cAlloc);
   if (this->buf.pbuf) {
      for (int ix = 0; ix < this->buf.cAlloc; ++ix) {
         str.sprintf_cat(!ix ? "[%g" : (ix == this->buf.cMax ? "|%g" : ",%g"), this->buf.pbuf[ix]);
         }
      str += "]";
      }

   MyString attr(pattr);
   if (flags & this->PubDecorateAttr)
      attr += "Debug";

   ad.Assign(pattr, str);
}

// Determine if enough time has passed to Advance the recent buffers,
// returns an advance count. 
// 
// note that the caller must insure that all 5 time_t values are preserved
// between calls to this function.  
//
int generic_stats_Tick(
   int    RecentMaxTime,
   int    RecentQuantum,
   time_t InitTime,
   time_t & LastUpdateTime,
   time_t & RecentTickTime,
   time_t & Lifetime,
   time_t & RecentLifetime)
{
   time_t now = time(NULL);

   // when working from freshly initialized stats, the first Tick should not Advance.
   //
   if (LastUpdateTime == 0)
      {
      LastUpdateTime = now;
      RecentTickTime = now;
      RecentLifetime = 0;
      return 0;
      }

   // whenever 'now' changes, we want to check to see how much time has passed
   // since the last Advance, and if that time exceeds the quantum, we advance
   // the recent buffers and update the PrevUpdateTime
   //
   int cTicks = 0;
   if (LastUpdateTime != now) 
      {
      time_t delta = now - RecentTickTime;

      // if delta from the previous update time exceeds the quantum, advance
      // and update the prev update time.
      if (delta >= RecentQuantum)
         {
         cTicks = delta / RecentQuantum;
         RecentTickTime = now - (delta % RecentQuantum);
         }

      time_t recent_time = (int)(RecentLifetime + now - LastUpdateTime);
      RecentLifetime = (recent_time < RecentMaxTime) ? recent_time : RecentMaxTime;
      LastUpdateTime = now;
      }

   Lifetime = now - InitTime;
   return cTicks;
}

// parse a configuration string in the form "ALL:opt, CAT:opt, ALT:opt"
// where opt can be one or more of 
//   0-3   verbosity level, 0 is least and 3 is most. default is usually 1
//   NONE  disable all 
//   ALL   enable all
//   R     enable Recent (default)
//   !R    disable Recent
//   D     enable Debug
//   !D    disable Debug (default)
//   Z     don't publish values markerd IF_NONZERO when their value is 0
//   !Z    ignore IF_NONZERO publishing flag 
// 
// return value is the PublishFlags that should be passed in to StatisticsPool::Publish
// for this category.
//
int generic_stats_ParseConfigString(
   const char * config, // name of the string parameter to read from the config file
   const char * pool_name, // name of the stats pool/category of stats to look for 
   const char * pool_alt,  // alternate name of the category to look for
   int          flags_def)  // default value for publish flags for this pool
{
    // special case, if there is no string, or the string is just "default", then
    // return the default flags
    if ( ! config || MATCH == strcasecmp(config,"DEFAULT"))
       return flags_def;

    // special case, if the string is empty, or the string is just "none", then
    // return 0 (disable all)
    if ( ! config[0] || MATCH == strcasecmp(config,"NONE"))
       return 0;

    // tokenize the list on , or space
    StringList items;
    items.initializeFromString(config);

    // if the config string is non-trivial, then it must contain either our pool_name
    // or pool_alt or "DEFAULT" or "ALL" or we do not publish this pool.
    int PublishFlags = 0;

    // walk the list, looking for items that match our pool name or the keyword DEFAULT or ALL
    // 
    items.rewind();
    while (const char * p = items.next()) {

       int flags = PublishFlags;
       const char * psep = strchr(p,':');
       if (psep) {
          size_t cch = psep - p;
          char sz[64];
          if (cch > COUNTOF(sz)) 
             continue;
          strncpy(sz, p, cch);
          sz[cch] = 0;
          if (strcasecmp(sz,pool_name) && strcasecmp(sz,pool_alt) && strcasecmp(sz,"DEFAULT") && strcasecmp(sz,"ALL"))
             continue;
       } else {
          if (strcasecmp(p,pool_name) && strcasecmp(p,pool_alt) && strcasecmp(p,"DEFAULT") && strcasecmp(p,"ALL"))
             continue;
       }

       // if we get to here, we found our pool name or "DEFAULT" or "ALL"
       // so we begin with our default flags
       flags = flags_def;

       // if there are any options, then parse them and modify the flags
       if (psep) {
          const char * popt = psep+1;
          if (MATCH == strcasecmp(popt,"NONE")) {
             flags = 0;
          } else {
             bool bang = false;
             const char * parse_error = NULL;
             while (popt[0]) {
                char ch = popt[0];
                if (ch >= '0' && ch <= '3') {
                   int level = (atoi(popt) * IF_BASICPUB) & IF_PUBLEVEL;
                   flags = (flags & ~IF_PUBLEVEL) | level;
                } else if (ch == '!') {
                   bang = true;
                } else if (ch == 'd' || ch == 'D') {
                   flags = bang ? (flags & ~IF_DEBUGPUB) : (flags | IF_DEBUGPUB);
                } else if (ch == 'r' || ch == 'R') {
                   flags = bang ? (flags & ~IF_RECENTPUB) : (flags | IF_RECENTPUB);
                } else if (ch == 'z' || ch == 'Z') {
                   flags = bang ? (flags & ~IF_NONZERO) : (flags | IF_NONZERO);
                } else if (ch == 'l' || ch == 'L') {
                   flags = bang ? (flags | IF_NOLIFETIME) : (flags &= ~IF_NOLIFETIME);
                } else {
                   if ( ! parse_error) parse_error = popt;
                }
                ++popt;
             }

             if (parse_error) {
                dprintf(D_ALWAYS, "Option '%s' invalid in '%s' when parsing statistics to publish. effect is %08X\n",
                        parse_error, p, flags);
             }
          }
       }

       PublishFlags = flags;
       dprintf(D_FULLDEBUG, "'%s' gives flags %08X for %s statistics\n", p, PublishFlags, pool_name);
    }

    return PublishFlags;
}

//----------------------------------------------------------------------------------------------
//
void stats_recent_counter_timer::Delete(stats_recent_counter_timer * probe) {
   delete probe;
}

void stats_recent_counter_timer::Publish(ClassAd & ad, const char * pattr, int flags) const
{
   if ((flags & IF_NONZERO) && (this->count.value == 0) && (this->count.recent == 0))
      return;

   MyString attr(pattr);
   MyString attrR("Recent");
   attrR += pattr;

   ClassAdAssign(ad, attr.Value(), this->count.value);
   ClassAdAssign(ad, attrR.Value(), this->count.recent);

   attr += "Runtime";
   attrR += "Runtime";
   ClassAdAssign(ad, attr.Value(), this->runtime.value);
   ClassAdAssign(ad, attrR.Value(), this->runtime.recent);

}

void stats_recent_counter_timer::PublishDebug(ClassAd & ad, const char * pattr, int flags) const
{
   if ( ! canStringBeUsedAsAttr(pattr))
      return;

   this->count.PublishDebug(ad, pattr, flags);

   MyString attr(pattr);
   attr += "Runtime";
   this->runtime.PublishDebug(ad, attr.Value(), flags);
}

// the Probe class is designed to be instantiated with
// the stats_entry_recent template,  i.e. stats_entry_recent<Probe>
// creates a probe that does full statistical sampling (min,max,avg,std)
// with an overall value and a recent-window'd value.
//
void Probe::Clear() 
{
   Count = 0; // value is use to store the count of samples.
   Max = std::numeric_limits<double>::min();
   Min = std::numeric_limits<double>::max();
   SumSq = Sum = 0.0;
}

double Probe::Add(double val) 
{ 
   Count += 1; // value is use to store the count of samples.
   if (val > Max) Max = val;
   if (val < Min) Min = val;
   Sum += val;
   SumSq += val*val;
   return Sum;
}

Probe & Probe::Add(const Probe & val) 
{ 
   if (val.Count >= 1) {
      Count += val.Count;
      if (val.Max > Max) Max = val.Max;
      if (val.Min < Min) Min = val.Min;
      Sum += val.Sum;
      SumSq += val.SumSq;
      }
   return *this;
}

double Probe::Avg() const 
{
   if (Count > 0) {
      return this->Sum / this->Count;
   } else {
      return this->Sum;
   }
}

double Probe::Var() const
{
   if (Count <= 1) {
      return this->Min;
   } else {
      // Var == (SumSQ - count*Avg*Avg)/(count -1)
      return (this->SumSq - this->Sum * (this->Sum / this->Count))/(this->Count - 1);
   }
}

double Probe::Std() const
{
   if (Count <= 1) {
      return this->Min;
   } else {
      return sqrt(this->Var());
   }
}

template <> void stats_entry_recent<Probe>::AdvanceBy(int cSlots) { 
   if (cSlots <= 0) 
      return;

   // remove the values associated with the slot being overwritten
   // from the aggregate value.
   while (cSlots > 0) {
      buf.Advance();
      --cSlots;
      }

   recent = buf.Sum();
}

template <> void stats_entry_recent< stats_histogram<int64_t> >::AdvanceBy(int cSlots)
{
   if (cSlots <= 0) 
      return;
   while (cSlots > 0) { buf.Advance(); --cSlots; }
   recent = buf.Sum();
}

template <> void stats_entry_recent< stats_histogram<double> >::AdvanceBy(int cSlots)
{
   if (cSlots <= 0) 
      return;
   while (cSlots > 0) { buf.Advance(); --cSlots; }
   recent = buf.Sum();
}

template <> void stats_entry_recent< stats_histogram<int> >::AdvanceBy(int cSlots)
{
   if (cSlots <= 0) 
      return;
   while (cSlots > 0) { buf.Advance(); --cSlots; }
   recent = buf.Sum();
}

template <> void stats_entry_recent< stats_histogram<int64_t> >::Publish(ClassAd& ad, const char * pattr, int flags) const
{
   MyString str;
   if (this->value.cItems <= 0) {
      str += "";
   } else {
      str += this->value.data[0];
      for (int ix = 1; ix < this->value.cItems+1; ++ix) {
         str += ", ";
         str += this->value.data[ix];
      }
   }

   ad.Assign(pattr, str);

   MyString attr(pattr);
   attr += "Set";
   ad.Assign(attr.Value(), "64Kb, 256Kb, 1Mb, 4Mb, 16Mb, 64Mb, 256Mb, 1Gb, 4Gb, 16Gb, 64Gb, 256Gb");
}


template <> int ClassAdAssign(ClassAd & ad, const char * pattr, const Probe& probe) {
   MyString attr(pattr);
   ad.Assign(pattr, probe.Count);
   attr += "Runtime";
   int ret = ad.Assign(attr.Value(), probe.Sum);
   if (probe.Count > 0)
      {
      int pos = attr.Length();
      attr += "Avg";
      ad.Assign(attr.Value(), probe.Avg());

      attr.replaceString("Avg","Min",pos);
      ad.Assign(attr.Value(), probe.Min);

      attr.replaceString("Min","Max",pos);
      ad.Assign(attr.Value(), probe.Max);

      attr.replaceString("Max","Std",pos);
      ad.Assign(attr.Value(), probe.Std());
      }
   return ret;
}

#include "utc_time.h"
void TestProbe()
{
   struct {
      stats_entry_recent<Probe> Runtime;
   } stats;

   stats.Runtime.SetRecentMax(5);

   double runtime = UtcTime::getTimeDouble();

   sleep(2);
   double now = UtcTime::getTimeDouble();
   stats.Runtime += (now - runtime);
   now = runtime;

   stats.Runtime.AdvanceBy(1);
}

//----------------------------------------------------------------------------------------------
// methods for the StatisticsPool class.  StatisticsPool is a collection of statistics that 
// share a recent time quantum and are intended to be published/Advanced/Cleared
// together.
//
StatisticsPool::~StatisticsPool()
{
   // first delete all of the publish entries.
   MyString name;
   pubitem item;
   pub.startIterations();
   while (pub.iterate(name,item))
      {
      pub.remove(name);
      if (item.fOwnedByPool && item.pattr)
         free((void*)item.pattr);
      }

   // then all of the probes. 
   void* probe;
   poolitem pi;
   pool.startIterations();
   while (pool.iterate(probe,pi))
      {
      pool.remove(probe);
      if (pi.Delete)
         pi.Delete(probe);
      }
}

int StatisticsPool::RemoveProbe (const char * name)
{
   pubitem item;
   if (pub.lookup(name, item) < 0)
      return 0;
   int ret =  pub.remove(name);

   void * probe = item.pitem;
   bool fOwnedByPool = item.fOwnedByPool;
   if (fOwnedByPool) {
      if (item.pattr) free((void*)item.pattr);
   }

   // remove the probe from the pool (if it's still there)
   poolitem pi;
   if (pool.lookup(probe, pi) >= 0) {
      pool.remove(probe);
      if (pi.Delete) {
         pi.Delete(probe);
      }
   }

   // clear out any dangling references to the probe in the pub list.
   // 
   /*
   StatisticsPool * pthis = const_cast<StatisticsPool*>(this);
   MyString key;
   pthis->pub.startIterations();
   while (pthis->pub.iterate(key,item)) {
      if (item.pitem == probe)
         pthis->pub.remove(key);
   }

   if (fOwnedByPool) {
      delete pitem;
   }
   */
   return ret;
}

void StatisticsPool::InsertProbe (
   const char * name,       // unique name for the probe
   int          unit,       // identifies the probe class/type
   void*        probe,      // the probe, usually a member of a class/struct
   bool         fOwned,     // probe and pattr string are owned by the pool
   const char * pattr,      // publish attribute name
   int          flags,      // flags to control publishing
   FN_STATS_ENTRY_PUBLISH fnpub, // publish method
   FN_STATS_ENTRY_ADVANCE fnadv, // Advance method
   FN_STATS_ENTRY_CLEAR   fnclr,  // Clear method
   FN_STATS_ENTRY_SETRECENTMAX fnsrm,
   FN_STATS_ENTRY_DELETE  fndel) // Destructor
{
   pubitem item = { unit, flags, fOwned, probe, pattr, fnpub };
   pub.insert(name, item);

   poolitem pi = { unit, fOwned, fnadv, fnclr, fnsrm, fndel };
   pool.insert(probe, pi);
}

void StatisticsPool::InsertPublish (
   const char * name,       // unique name for the probe
   int          unit,       // identifies the probe class/type
   void*        probe,      // the probe, usually a member of a class/struct
   bool         fOwned,     // probe and pattr string are owned by the pool
   const char * pattr,      // publish attribute name
   int          flags,      // flags to control publishing
   FN_STATS_ENTRY_PUBLISH fnpub) // publish method
{
   pubitem item = { unit, flags, fOwned, probe, pattr, fnpub };
   pub.insert(name, item);
}

double StatisticsPool::SetSample(const char * probe_name, double sample)
{
   return sample;
}

int StatisticsPool::SetSample(const char * probe_name, int sample)
{
   return sample;
}

int64_t StatisticsPool::SetSample(const char * probe_name, int64_t sample)
{
   return sample;
}

int StatisticsPool::Advance(int cAdvance)
{
   if (cAdvance <= 0)
      return cAdvance;

   void* pitem;
   poolitem item;
   pool.startIterations();
   while (pool.iterate(pitem,item))
      {
      if (pitem && item.Advance) {
         stats_entry_base * probe = (stats_entry_base *)pitem;
         (probe->*(item.Advance))(cAdvance);
         }
      }
   return cAdvance;
}

void StatisticsPool::Clear()
{
   void* pitem;
   poolitem item;
   pool.startIterations();
   while (pool.iterate(pitem,item))
      {
      if (pitem && item.Clear) {
         stats_entry_base * probe = (stats_entry_base *)pitem;
         (probe->*(item.Clear))();
         }
      }
}

void StatisticsPool::ClearRecent()
{
   EXCEPT("StatisticsPool::ClearRecent has not been implemented");
}

void StatisticsPool::SetRecentMax(int window, int quantum)
{
   int cRecent = quantum ? window / quantum : window;

   void* pitem;
   poolitem item;
   pool.startIterations();
   while (pool.iterate(pitem,item))
      {
      if (pitem && item.SetRecentMax) {
         stats_entry_base * probe = (stats_entry_base *)pitem;
         (probe->*(item.SetRecentMax))(cRecent);
         }
      }
}

void StatisticsPool::Publish(ClassAd & ad, int flags) const
{
   pubitem item;
   MyString name;

   // boo! HashTable doesn't support const, so I have to remove const from this
   // to make the compiler happy.
   StatisticsPool * pthis = const_cast<StatisticsPool*>(this);
   pthis->pub.startIterations();
   while (pthis->pub.iterate(name,item)) 
      {
      // check various publishing flags to decide whether to call the Publish method
      if (!(flags & IF_DEBUGPUB) && (item.flags & IF_DEBUGPUB)) continue;
      if (!(flags & IF_RECENTPUB) && (item.flags & IF_RECENTPUB)) continue;
      if ((flags & IF_PUBKIND) && (item.flags & IF_PUBKIND) && !(flags & item.flags & IF_PUBKIND)) continue;
      if ((item.flags & IF_PUBLEVEL) > (flags & IF_PUBLEVEL)) continue;

      // don't pass the item's IF_NONZERO flag through unless IF_NONZERO is enabled
      int item_flags = (flags & IF_NONZERO) ? item.flags : (item.flags & ~IF_NONZERO);

      if (item.Publish) {
         stats_entry_base * probe = (stats_entry_base *)item.pitem;
         (probe->*(item.Publish))(ad, item.pattr ? item.pattr : name.Value(), item_flags);
         }
      }
}

void StatisticsPool::Unpublish(ClassAd & ad) const
{
   pubitem item;
   MyString name;

   // boo! HashTable doesn't support const, so I have to remove const from this
   // to make the compiler happy.
   StatisticsPool * pthis = const_cast<StatisticsPool*>(this);
   pthis->pub.startIterations();
   while (pthis->pub.iterate(name,item)) 
      {
      ad.Delete(item.pattr ? item.pattr : name.Value());
      }
}

// some compiler/linkers need us to force template instantiation
// since the template code isn't in a header file.
// force the static members to be instantiated in the library.
//
void generic_stats_force_refs()
{
   stats_entry_recent<int>* pi = NULL;
   stats_entry_recent<long>* pl = NULL;
   stats_entry_recent<int64_t>* pt = NULL;
   stats_entry_recent<double>* pd = NULL;
   stats_entry_recent< stats_histogram<int64_t> >* ph = new stats_entry_recent< stats_histogram<int64_t> >();
   stats_recent_counter_timer* pc = NULL;

   ph->value.set_levels(0, NULL);

   StatisticsPool dummy;
   dummy.GetProbe<stats_entry_recent<int> >("");
   dummy.GetProbe<stats_entry_recent<int64_t> >("");
   dummy.GetProbe<stats_entry_recent<long> >("");
   dummy.GetProbe<stats_entry_recent<double> >("");
   dummy.GetProbe<stats_recent_counter_timer>("");

   dummy.NewProbe<stats_entry_recent<int> >("");
   dummy.NewProbe<stats_entry_recent<int64_t> >("");
   dummy.NewProbe<stats_entry_recent<long> >("");
   dummy.NewProbe<stats_entry_recent<double> >("");
   dummy.NewProbe<stats_recent_counter_timer>("");
   //dummy.Add<stats_entry_recent<int>*>("",0,pi,0,"",NULL);
   //dummy.Add<stats_entry_recent<long>*>("",0,pl,0,"",NULL);
   //dummy.Add<stats_entry_recent<int64_t>*>("",0,pt,0,"",NULL);
   //dummy.Add<stats_recent_counter_timer*>("",0,pc,0,"",NULL);
   dummy.AddProbe("",pi,NULL,0);
   dummy.AddProbe("",pl,NULL,0);
   dummy.AddProbe("",pt,NULL,0);
   dummy.AddProbe("",pc,NULL,0);
   dummy.AddProbe("",pd,NULL,0);
   dummy.AddProbe("",ph,NULL,0);
};

template <class T>
stats_histogram<T>::~stats_histogram()
{
		delete [] data;
		delete [] levels;
}
template<class T>
stats_histogram<T>::stats_histogram(int num,const T* ilevels) 
   : cItems(num), data(NULL), levels(NULL)
{
	if(ilevels){
		data = new int[cItems+2];
		levels = new T[cItems];
		for(int i=0;i<cItems;++i){
			levels[i] = ilevels[i];
			data[i] = 0;
		}
        data[cItems] = 0;
	}
}

template<class T>
bool stats_histogram<T>::set_levels(int num, const T* ilevels)
{
   // early out if the levels being set it the same as what we currently use.
   if (num == cItems && ilevels && levels) {
      bool match = false;
      for (int i=0;i<cItems;++i) {
         if (levels[i] != ilevels[i]) {
            match = false;
            break;
         }
      }
      if (match) return false;
   }

   delete [] data;
   delete [] levels;
   cItems = num;
   if (num > 0) {
      data = new int[cItems+1];
      levels = new T[cItems];
      for(int i=0;i<cItems;++i) {
		levels[i] = ilevels[i];
		data[i] = 0;
      }
      data[cItems] = 0;
   }
   return true;
}

template<class T>
T stats_histogram<T>::get_level(int n) const
{
	if(0 <= n && n < cItems){
		return levels[n];
	}
	if(n >= cItems) {
		return std::numeric_limits<T>::max();
	} else {
		return std::numeric_limits<T>::min();
	}
}

template<class T>
bool stats_histogram<T>::set_level(int n, T val)
{
	bool ret = false;
	if(0 < n && n < cItems - 1){
		if(val > levels[n-1] && val < levels[n+1]) {
			levels[n] = val;
			Clear();
			ret = true;
		}
	} else if(n == 0 && cItems >=2 ) {
		if(val < levels[1]){
			levels[0] = val;
			Clear();
			ret = true;
		}
	} else if(n == cItems - 1 && cItems >= 2) {
		if(val > levels[n-1]) {
			levels[n] = val;
			Clear();
			ret = true;
		}
	}
	return ret;
}	

template<class T>
void stats_histogram<T>::Clear()
{
	for(int i=0;i<cItems;++i){
		data[i] = 0;
	}
	return true;
}

template<class T>
T stats_histogram<T>::Add(T val)
{
	if(val < levels[0]){
		++data[0];
	} else if(val >= levels[cItems - 1]){
		++data[cItems];
	} else {
		int count = cItems - 1;
		for(int i=1;i<=count;++i){
			if(val >= levels[i-1] && val < levels[i]){
				++data[i];
			}
		}
	}
	return val;
}

template<class T>
stats_histogram<T>& stats_histogram<T>::operator+=(const stats_histogram<T>& sh)
{
   // if the input histogram is null, there is nothing to do.
   if (sh.cItems <= 0) {
      return *this;
   }

   // if the current histogram is null, take on the size and levels of the input
   if (this->cItems <= 0) {
      this->set_levels(sh.cItems, sh.levels);
   }

   // to add histograms, they must both be the same size (and have the same
   // limits array as well, should we check that?)
   if (this->cItems != sh.cItems) {
      EXCEPT("attempt to add histogram of %d items to histogram of %d items", 
             sh.cItems, this->cItems);
   }

   for (int i = 0; i < cItems-1; ++i) {
      this->data[i] += sh.data[i];
   }

   return *this;
}

/*
template<class T>
void stats_histogram<T>::Publish(ClassAd& ad, const char* pattr, int flags) const 
{
	
}
*/

//
// This is how you use the generic_stats functions.
#ifdef UNIT_TESTS

class TestStats {
public:
   // InitTime should be the first data member, and RecentWindowMax the last data member..
   time_t InitTime;            // last time we init'ed the structure

   time_t StatsLastUpdateTime; // the prior time that statistics were updated. 
   int    RecentWindowMax;     // size of the time window over which RecentXXX values are calculated.
   time_t RecentStatsTickTime; // time of the latest recent buffer Advance

   // published values
   time_t StatsLifetime;       // last time we init'ed the structure
   time_t RecentStatsLifetime; // actual time span of current DCRecentXXX data.

   stats_entry_recent<int>    Signals; 
   stats_entry_recent<time_t> SignalRuntime;

   StatisticsPool Pool;           // pool of statistics probes and Publish attrib names

   void Init();
   void Clear();
   void Tick(); // call this when time may have changed to update StatsLastUpdateTime, etc.
   void SetWindowSize(int window);
   void Publish(ClassAd & ad) const;
   void Unpublish(ClassAd & ad) const;
};


const int test_stats_window_quantum = 60;

void TestStats::SetWindowSize(int window)
{
   this->RecentWindowMax = window;
   Pool.SetRecentMax(window, test_stats_window_quantum);
}

void TestStats::Init()
{
   Clear();
   this->RecentWindowMax = test_stats_window_quantum; 

   STATS_POOL_ADD_VAL_PUB_RECENT(Pool, "Test", Signals, 0);
   STATS_POOL_ADD_VAL_PUB_RECENT(Pool, "Test", SignalRuntime, 0);
}

void TestStats::Clear()
{
   this->InitTime = time(NULL);
   this->StatsLifetime = 0;
   this->StatsLastUpdateTime = 0;
   this->RecentStatsTickTime = 0;
   this->RecentStatsLifetime = 0;
}

void TestStats::Tick()
{
   int cAdvance = generic_stats_Tick(
      this->RecentWindowMax,     // RecentMaxTime
      test_stats_window_quantum, // RecentQuantum
      this->InitTime,
      this->StatsLastUpdateTime,
      this->RecentStatsTickTime,
      this->StatsLifetime,
      this->RecentStatsLifetime);
   if (cAdvance > 0)
      Pool.Advance(cAdvance);
}

void TestStats::Unpublish(ClassAd & ad) const
{
   Pool.Publish(ad, 0);
}


void TestStats::Publish(ClassAd & ad) const
{
   Pool.Unpublish(ad);
}
#endif