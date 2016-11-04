// This work is licensed under the Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License.
// To view a copy of this license, visit http://creativecommons.org/licenses/by-nc-nd/4.0/ or send a letter to
// Creative Commons, PO Box 1866, Mountain View, CA 94042, USA.


#include <boost/shared_ptr.hpp>
#include <boost/pointer_cast.hpp>
#include <string>
#include "mrgsolve.h"
#include "odeproblem.h"
#include "dataobject.h"
#include "RcppInclude.h"


#define CRUMP(a) Rcpp::stop(a)
#define REP(a)   Rcpp::Rcout << #a << std::endl;
#define nREP(a)  Rcpp::Rcout << a << std::endl;
#define say(a)   Rcpp::Rcout << a << std::endl;


/** Perform a simulation run.
 * 
 * @param parin list of data and options for the simulation
 * @param inpar numeric parameter values
 * @param parnames parameter names
 * @param init numeric initial values
 * @param cmtnames compartment names
 * @param capture indices in capture vector to actually get
 * @param funs list of pointer addresses to model functions generated by 
 * getNativeSymbolInfo()
 * @param data the main data set
 * @param idata the idata data aset
 * @param OMEGA between-ID normal random effects
 * @param SIGMA within-ID normal random effects
 * @return list containing matrix of simulated data and a character vector of
 * tran names that may have been carried into the output
 *
 */
// [[Rcpp::export]]
Rcpp::List DEVTRAN(const Rcpp::List parin,
                   const Rcpp::NumericVector& inpar,
                   const Rcpp::CharacterVector& parnames,
                   const Rcpp::NumericVector& init,
                   Rcpp::CharacterVector& cmtnames,
                   const Rcpp::IntegerVector& capture,
                   const Rcpp::List& funs,
                   const Rcpp::NumericMatrix& data,
                   const Rcpp::NumericMatrix& idata,
                   Rcpp::NumericMatrix& OMEGA,
                   Rcpp::NumericMatrix& SIGMA,
                   Rcpp::Environment envir) {
  
  const unsigned int verbose  = Rcpp::as<int>    (parin["verbose"]);
  const bool debug            = Rcpp::as<bool>   (parin["debug"]);
  const int digits            = Rcpp::as<int>    (parin["digits"]);
  const double tscale         = Rcpp::as<double> (parin["tscale"]);
  const bool obsonly          = Rcpp::as<bool>   (parin["obsonly"]);
  bool obsaug                 = Rcpp::as<bool>   (parin["obsaug"] );
  obsaug = obsaug & (data.nrow() > 0);
  const int  recsort          = Rcpp::as<int>    (parin["recsort"]);
  const bool filbak           = Rcpp::as<bool>   (parin["filbak"]);
  const double mindt          = Rcpp::as<double> (parin["mindt"]);

  // Create data objects from data and idata
  dataobject dat(data,parnames);
  dat.map_uid();
  dat.locate_tran();
  
  dataobject idat(idata, parnames, cmtnames);
  idat.idata_row();
  
  // Number of individuals in the data set
  const unsigned int NID = dat.nid();
  const int nidata = idat.nrow();
  
  int j = 0;
  unsigned int k = 0;
  unsigned int crow = 0; 
  size_t h = 0;
  
  bool put_ev_first = false;
  bool addl_ev_first = true;
  
  switch (recsort) {
  case 1:
    break;
  case 2:
    put_ev_first = false;
    addl_ev_first = false;
    break;
  case 3:
    put_ev_first = true;
    addl_ev_first = true;
    break;
  case 4:
    put_ev_first = true;
    addl_ev_first = false;
    break;
  default:
    CRUMP("recsort must be 1, 2, 3, or 4.");
  }
  
  // Requested compartments  
  Rcpp::IntegerVector request = parin["request"];
  const unsigned int nreq = request.size();
  
  // Columns from the data set to carry:
  Rcpp::CharacterVector data_carry_ = Rcpp::as<Rcpp::CharacterVector >(parin["carry_data"]);
  const Rcpp::IntegerVector data_carry =  dat.get_col_n(data_carry_);
  const unsigned int n_data_carry = data_carry.size();
  
  // Columns from the idata set to carry:
  unsigned int n_idata_carry=0;
  Rcpp::IntegerVector idata_carry;  
  if(nidata > 0) {
    Rcpp::CharacterVector idata_carry_ = Rcpp::as<Rcpp::CharacterVector >(parin["carry_idata"]);
    idata_carry =  idat.get_col_n(idata_carry_);
    n_idata_carry = idata_carry.size();
    dat.check_idcol(idat);
  }
  
  // Tran Items to carry:
  Rcpp::CharacterVector tran_carry = Rcpp::as<Rcpp::CharacterVector >(parin["carry_tran"]);
  const unsigned int n_tran_carry = tran_carry.size();
  
  // Captures
  const unsigned int n_capture  = capture.size()-1;
  
  // Create odeproblem object  
  odeproblem *prob  = new odeproblem(inpar, init, funs, capture.at(0));
  arma::mat OMEGA_(OMEGA.begin(),OMEGA.nrow(),OMEGA.ncol(),false);
  arma::mat SIGMA_(SIGMA.begin(),SIGMA.nrow(),SIGMA.ncol(),false);
  prob->omega(OMEGA_);
  prob->sigma(SIGMA_);
  prob->copy_parin(parin);
  prob->pass_envir(&envir);
  const unsigned int neq = prob->neq();
  
  // Allocate the record list:
  recstack a(NID);
  
  // dataobject.cpp
  // Extract data records from the data set
  // Track the number of observations and events
  unsigned int obscount = 0;
  unsigned int evcount = 0;
  dat.get_records(a, NID, neq, obscount, evcount, obsonly, debug);
  
  // Observations from stime will always come after events;
  // unsigned int nextpos = 0; warnings
  // Vector of simulation times
  // only active if no evid=0 records in data (cleared out in that case).
  dvec mtimes = Rcpp::as<dvec>(parin["mtime"]);
  
  // Need this for later
  int nextpos = put_ev_first ?  (data.nrow() + 10) : -100;
  
  // ******* START TGRID SECTION *******
  // Take apart tgrid matrix and create observation event objects
  // with generic ID
  // Only  do this if we need to insert observations into the stack
  if((obscount == 0) || (obsaug)) {
    
    // Padded times
    dvec ptimes = Rcpp::as<dvec>(parin["ptimes"]);
    
    // Matrix of designs
    Rcpp::NumericMatrix tgrid = Rcpp::as<Rcpp::NumericMatrix>(parin["tgridmatrix"]);
    
    // Vector of length idata.nrow() that maps each ID to a design
    // Already has C indexing
    Rcpp::IntegerVector tgridi = Rcpp::as<Rcpp::IntegerVector>(parin["whichtg"]);
    
    if(tgridi.size() == 0) tgridi = Rcpp::rep(0,NID);
    
    if(tgridi.size() < NID) CRUMP("Length of design indicator less than NID.");
    
    if(max(tgridi) >= tgrid.ncol()) {
      Rcpp::stop("Insufficient number of designs specified for this problem.");
    }
    
    // Number of non-na times in each design
    std::vector<int> tgridn;
    if(tgrid.ncol() > 1) {
      for(int i = 0; i < tgrid.ncol(); ++i) {
        tgridn.push_back(Rcpp::sum(!Rcpp::is_na(tgrid(Rcpp::_,i))));
      }
    } else {
      tgridn.push_back(tgrid.nrow());
    }
    
    // Create a common dictionary of observation events
    // Vector of vectors
    // Outer vector: length = number of designs
    // Inner vector: length = number of times in that design
    std::vector<std::vector<rec_ptr> > designs;
    
    designs.reserve(tgridn.size());
    
    for(size_t i = 0; i < tgridn.size(); ++i) {
      
      std::vector<rec_ptr> z;
      
      z.reserve(tgridn[i]);
      
      for(int j = 0; j < tgridn[i]; ++j) { 
        rec_ptr obs = boost::make_shared<datarecord>(
          tgrid(j,i),nextpos,true
        );
        z.push_back(obs); 
      }
      designs.push_back(z);
    }
    
    double id;
    size_t n;
    size_t m = ptimes.size();
    
    for(recstack::iterator it = a.begin(); it != a.end(); ++it) {
      
      id = dat.get_uid(it-a.begin());
      
      j = idat.get_idata_row(id);
      
      n = tgridn[tgridi[j]];
      
      it->reserve((it->size() + n + m + 10));
      
      for(h=0; h < n; h++) {
        it->push_back(designs.at(tgridi[j]).at(h));
        ++obscount;
      } // done adding stimes;
      
      for(h=0; h < m; h++) {
        rec_ptr obs = boost::make_shared<datarecord>(ptimes[h],nextpos,false);
        it->push_back(obs);
      }
      // sort the records by time and original position 
      std::sort(it->begin(), it->end(), CompRec());
    }
  }
  // ******* END TGRID SECTION ******* 
  
  
  
  // Create results matrix:
  //  rows: ntime*nset
  //  cols: rep, time, eq[0], eq[1], ..., yout[0], yout[1],...
  const unsigned int NN = obsonly ? obscount : (obscount + evcount);
  const unsigned int n_out_col  = 2 + n_tran_carry + n_data_carry + n_idata_carry + nreq + n_capture;
  Rcpp::NumericMatrix ans(NN,n_out_col);
  const unsigned int tran_carry_start = 2;
  const unsigned int data_carry_start = tran_carry_start + n_tran_carry;
  const unsigned int idata_carry_start = data_carry_start + n_data_carry;
  const unsigned int req_start = idata_carry_start+n_idata_carry;
  const unsigned int capture_start = req_start+nreq;
  
  // SIMULATE ETA AND EPS
  //   - Need NN for this
  const unsigned int neta = OMEGA.nrow();
  arma::mat eta;
  if(neta > 0) {
    eta = prob->mv_omega(NID);
    prob->neta(neta);
  }
  
  const unsigned int neps = SIGMA.nrow();
  arma::mat eps;
  if(neps > 0) {
    eps = prob->mv_sigma(NN);
    prob->neps(neps);
  }
  
  // Carry along TRAN data items (evid, amt, ii, ss, rate)
  Rcpp::CharacterVector tran_names;
  if(n_tran_carry > 0) {
    
    Rcpp::CharacterVector::iterator tcbeg  = tran_carry.begin();
    Rcpp::CharacterVector::iterator tcend  = tran_carry.end();
    
    // items in tran_carry are always lc
    const bool carry_evid = std::find(tcbeg,tcend, "evid")  != tcend;
    const bool carry_cmt =  std::find(tcbeg,tcend, "cmt")   != tcend;
    const bool carry_amt =  std::find(tcbeg,tcend, "amt")   != tcend;
    const bool carry_ii =   std::find(tcbeg,tcend, "ii")    != tcend;
    const bool carry_addl = std::find(tcbeg,tcend, "addl")  != tcend;
    const bool carry_ss =   std::find(tcbeg,tcend, "ss")    != tcend;
    const bool carry_rate = std::find(tcbeg,tcend, "rate")  != tcend;
    const bool carry_aug  = std::find(tcbeg,tcend, "a.u.g") != tcend;
    
    if(carry_evid) tran_names.push_back("evid");
    if(carry_amt)  tran_names.push_back("amt");
    if(carry_cmt)  tran_names.push_back("cmt");
    if(carry_ss)   tran_names.push_back("ss");
    if(carry_ii)   tran_names.push_back("ii");
    if(carry_addl) tran_names.push_back("addl");
    if(carry_rate) tran_names.push_back("rate");
    if(carry_aug)  tran_names.push_back("a.u.g");
    
    
    crow = 0; // current output row
    int n = 0;
    for(recstack::const_iterator it = a.begin(); it !=a.end(); ++it) {
      for(reclist::const_iterator itt = it->begin(); itt != it->end(); ++itt) {
        if(!(*itt)->output()) continue;
        n = 0;
        if(carry_evid) {ans(crow,n+2) = (*itt)->evid();                     ++n;}
        if(carry_amt)  {ans(crow,n+2) = (*itt)->amt();                      ++n;}
        if(carry_cmt)  {ans(crow,n+2) = (*itt)->cmt();                      ++n;}
        if(carry_ss)   {ans(crow,n+2) = (*itt)->ss();                       ++n;}
        if(carry_ii)   {ans(crow,n+2) = (*itt)->ii();                       ++n;}
        if(carry_addl) {ans(crow,n+2) = (*itt)->addl();                     ++n;}
        if(carry_rate) {ans(crow,n+2) = (*itt)->rate();                     ++n;}
        if(carry_aug)  {ans(crow,n+2) = ((*itt)->pos()==nextpos) && obsaug; ++n;}
        ++crow;
      }
    }
  }
  
  
  // Carry items from data or idata
  if(((n_idata_carry > 0) || (n_data_carry > 0)) ) {
    dat.carry_out(a,ans,idat,data_carry,data_carry_start,
                  idata_carry,idata_carry_start);
  }
  
  if(verbose||debug)  Rcpp::Rcout << "Solving ... ";
  
  double tto, tfrom;
  crow = 0;
  int this_cmt = 0;
  
  // The current difference between tto and tfrom
  double dt = 0;
  double id = 0;
  double maxtime = 0;
  double biofrac = 1.0;
  int this_idata_row = 0;
  
  // i is indexing the subject, j is the record
  
  // LOOP ACROSS IDS:
  // tgrid observations have generic ID
  // We must first figure out the ID we are working with
  // and assign in the object
  for(size_t i=0; i < a.size(); ++i) {
    
    tfrom = a[i].front()->time();
    maxtime = a[i].back()->time();
    
    id = dat.get_uid(i);
    
    this_idata_row  = idat.get_idata_row(id);
  
    prob->reset_newid(id);
    
    if(i==0) prob->newind(0);
    
    // Copy eta/eps values for this ID
    for(k=0; k < neta; ++k) prob->eta(k,eta(i,k));
    for(k=0; k < neps; ++k) prob->eps(k,eps(crow,k));
    
    // Refresh parameters in data:
    dat.reload_parameters(inpar,prob);
    
    //Copy parameters from idata
    idat.copy_parameters(this_idata_row,prob);
    
    if(a[i][0]->from_data()) {
      // If this record is from the data set, copy parameters from data
      dat.copy_parameters(a[i][0]->pos(), prob);
    } else {
      if(filbak) {
        dat.copy_parameters(dat.start(i),prob);
      }
    }
    
    // Calculate initial conditions:
    prob->y_init(init);
    
    // Copy initials from idata
    idat.copy_inits(this_idata_row,prob);
    
    // Call $MAIN
    prob->init_call(tfrom);
    
    // mtime
    if(mtimes.size() > 0) {
      add_mtime(a[i], mtimes, prob->mtime(),(debug||verbose));
    }

    // LOOP ACROSS EACH RECORD for THIS ID:
    for(size_t j=0; j < a[i].size(); ++j) {
      
      if(crow == NN) continue;
      
      if(j != 0) prob->newind(2);
      
      rec_ptr this_rec = a[i][j];
      
      this_rec->id(id);
      
      // Fill in the remaining records once system is turned off
      if(prob->systemoff()) {
        if(this_rec->output()) {
          if(prob->CFONSTOP()) {
            ans(crow,0) = this_rec->id();
            ans(crow,1) = this_rec->time();
            for(k=0; k < n_capture; ++k) {
              ans(crow,(k+capture_start)) = prob->capture(capture[k+1]);
            }
            for(k=0; k < nreq; ++k) {
              ans(crow,(k+req_start)) = prob->y(request[k]);
            }
          } else {
            ans(crow,0) = NA_REAL;
          }
          ++crow;
        }
        continue;
      } // End if(systemoff)
      
      // For this ID, we already have parameters from the first row; only update when
      // we come across a record from the data set
      if(this_rec->from_data()) {
        dat.copy_parameters(this_rec->pos(), prob);
      }
      
      tto = this_rec->time();
      dt  = (tto-tfrom)/(tfrom == 0.0 ? 1.0 : tfrom);
      
      // If tto is too close to tfrom, set tto to tfrom
      // dt is never negative; dt will never be < mindt when mindt==0
      if((dt > 0.0) && (dt < mindt)) { 
        tto = tfrom;
      }
      
      // Only copy in a new eps value if we are actually advancing in time:
      if(tto > tfrom) {
        for(k = 0; k < neps; ++k) {
          prob->eps(k,eps(crow,k));
        }
      }
      
      prob->evid(this_rec->evid());
      
      prob->init_call_record(tto);
      
      // Schedule ADDL and infusion end times
      if((this_rec->is_event()) && (this_rec->from_data())) {
        
        // Grab Bioavailability
        biofrac = prob->fbio(abs(this_rec->cmt())-1);
        
        if(biofrac < 0) {
          CRUMP("mrgsolve: Bioavailability fraction is less than zero.");
        }
        
        this_rec->fn(biofrac);
        
        // We already found an negative rate or duration in the data set.
        if(this_rec->rate() < 0) {
          if(this_rec->rate() == -1) {
            this_cmt = this_rec->cmt()-1;
            if(prob->rate(this_cmt) <= 0) {
              Rcpp::stop("Invalid infusion setting: rate (R_CMT).");
            }
            this_rec->rate(prob->rate(this_cmt));
          }
          
          if(this_rec->rate() == -2) {
            this_cmt = this_rec->cmt()-1;
            if(prob->dur(this_cmt) <= 0) {
              Rcpp::stop("Invalid infusion setting: duration (D_CMT).");
            }
            this_rec->rate(this_rec->amt() * biofrac / prob->dur(this_cmt));
          }
        } // End ev->rate() < 0
        
        // If alag set for this compartment
        // spawn a new event with no output and time modified by alag
        // disarm this event
        if((prob->alag(this_rec->cmt()) > mindt)) {
          
          rec_ptr newev = boost::make_shared<datarecord>(*this_rec);
          newev->pos(-1200);
          newev->phantom_rec();
          newev->time(this_rec->time() + prob->alag(this_rec->cmt()));
          newev->fn(biofrac);
          
          this_rec->unarm();
          
          reclist::iterator it = a[i].begin()+j;
          advance(it,1);
          a[i].insert(it,newev);
          newev->schedule(a[i], maxtime, addl_ev_first);
          std::sort(a[i].begin()+j,a[i].end(),CompRec());
          
        } else {
          this_rec->schedule(a[i], maxtime, addl_ev_first); 
          if(this_rec->needs_sorting()) {
            std::sort(a[i].begin()+j+1,a[i].end(),CompRec());
          }
        }
      }
      
      prob->advance(tfrom,tto);
      
      if(this_rec->evid() != 2) {
        this_rec->implement(prob);
      }
      
      // Write save values to output matrix:
      prob->table_call();
      
      if(this_rec->output()) {
        
        // Write out ID and time
        ans(crow,0) = this_rec->id();
        ans(crow,1) = this_rec->time();
        
        // Write out captured items
        k = 0;
        for(unsigned int i=0; i < n_capture; ++i) {
          ans(crow,k+capture_start) = prob->capture(capture[i+1]);
          ++k;
        }
        
        // Write out requested compartments
        for(k=0; k < nreq; ++k) {
          ans(crow,(k+req_start)) = prob->y(request[k]);
        }
        
        ++crow; // this must inside check to output
      } // end if ouput()
      
      
      // Reset or other events:
      if(this_rec->evid()==2) {
        this_rec->implement(prob);
      }
      
      // Move tto to tfrom
      tfrom = tto;
    }
  }
  
  // Significant digits in simulated variables and outputs too
  if(digits > 0) {
    for(int i=req_start; i < ans.ncol(); ++i) {
      ans(Rcpp::_, i) = signif(ans(Rcpp::_,i), digits);
    }
  }
  if((tscale != 1) && (tscale >= 0)) {
    ans(Rcpp::_,1) = ans(Rcpp::_,1) * tscale;
  }
  
  
  // Clean up
  delete prob;
  
  return Rcpp::List::create(Rcpp::Named("data") = ans,
                            Rcpp::Named("trannames") = tran_names);
  
  
}


