#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "nlopt.h"
#include "nlopt-util.h"
#include "config.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

/*************************************************************************/

#ifdef INFINITY
#  define MY_INF INFINITY
#else
#  define MY_INF HUGE_VAL
#endif

static int my_isinf(double x) {
     return fabs(x) >= HUGE_VAL * 0.99
#ifdef HAVE_ISINF
	  || isinf(x)
#endif
	  ;
}

#ifndef HAVE_ISNAN
static int my_isnan(double x) { return x != x; }
#  define isnan my_isnan
#endif

/*************************************************************************/

void nlopt_version(int *major, int *minor, int *bugfix)
{
     *major = MAJOR_VERSION;
     *minor = MINOR_VERSION;
     *bugfix = BUGFIX_VERSION;
}

/*************************************************************************/

static const char nlopt_algorithm_names[NLOPT_NUM_ALGORITHMS][128] = {
     "DIRECT (global)",
     "DIRECT-L (global)",
     "Randomized DIRECT-L (global)",
     "Original DIRECT version (global)",
     "Original DIRECT-L version (global)",
     "Subplex (local)",
     "StoGO (global)",
     "StoGO with randomized search (global)",
     "Low-storage BFGS (LBFGS) (local)"
};

const char *nlopt_algorithm_name(nlopt_algorithm a)
{
     if (a < 0 || a >= NLOPT_NUM_ALGORITHMS) return "UNKNOWN";
     return nlopt_algorithm_names[a];
}

/*************************************************************************/

static int nlopt_srand_called = 0;
void nlopt_srand(unsigned long seed) {
     nlopt_srand_called = 1;
     nlopt_init_genrand(seed);
}

void nlopt_srand_time() {
     nlopt_srand(nlopt_time_seed());
}

/*************************************************************************/

typedef struct {
     nlopt_func f;
     void *f_data;
     const double *lb, *ub;
} nlopt_data;

#include "subplex.h"

static double f_subplex(int n, const double *x, void *data_)
{
     int i;
     nlopt_data *data = (nlopt_data *) data_;
     double f;

     /* subplex does not support bound constraints, but it supports
	discontinuous objectives so we can just return Inf for invalid x */
     for (i = 0; i < n; ++i)
	  if (x[i] < data->lb[i] || x[i] > data->ub[i])
	       return MY_INF;

     f = data->f(n, x, NULL, data->f_data);
     return (isnan(f) ? MY_INF : f);
}

#include "direct.h"

static double f_direct(int n, const double *x, int *undefined, void *data_)
{
     nlopt_data *data = (nlopt_data *) data_;
     double f;
     f = data->f(n, x, NULL, data->f_data);
     *undefined = isnan(f) || my_isinf(f);
     return f;
}

#include "stogo.h"

#include "l-bfgs-b.h"

#include "cdirect.h"

/*************************************************************************/

/* same as nlopt_minimize, but xtol_abs is required to be non-NULL */
static nlopt_result nlopt_minimize_(
     nlopt_algorithm algorithm,
     int n, nlopt_func f, void *f_data,
     const double *lb, const double *ub, /* bounds */
     double *x, /* in: initial guess, out: minimizer */
     double *fmin, /* out: minimum */
     double fmin_max, double ftol_rel, double ftol_abs,
     double xtol_rel, const double *xtol_abs,
     int maxeval, double maxtime)
{
     int i;
     nlopt_data d;
     nlopt_stopping stop;

     /* some basic argument checks */
     if (n <= 0 || !f || !lb || !ub || !x || !fmin)
	  return NLOPT_INVALID_ARGS;

     d.f = f;
     d.f_data = f_data;
     d.lb = lb;
     d.ub = ub;

     /* make sure rand generator is inited */
     if (!nlopt_srand_called)
	  nlopt_srand_time(); /* default is non-deterministic */

     /* check bound constraints */
     for (i = 0; i < n; ++i)
	  if (lb[i] > ub[i] || x[i] < lb[i] || x[i] > ub[i])
	       return NLOPT_INVALID_ARGS;

     stop.n = n;
     stop.fmin_max = (isnan(fmin_max) || (my_isinf(fmin_max) && fmin_max < 0))
	  ? -MY_INF : fmin_max;
     stop.ftol_rel = ftol_rel;
     stop.ftol_abs = ftol_abs;
     stop.xtol_rel = xtol_rel;
     stop.xtol_abs = xtol_abs;
     stop.nevals = 0;
     stop.maxeval = maxeval;
     stop.maxtime = maxtime;
     stop.start = nlopt_seconds();

     switch (algorithm) {
	 case NLOPT_GLOBAL_DIRECT:
	 case NLOPT_GLOBAL_DIRECT_L: 
	 case NLOPT_GLOBAL_DIRECT_L_RANDOMIZED: 
	      return cdirect(n, f, f_data, lb, ub, x, fmin, &stop, 0.0, 
			     (algorithm != NLOPT_GLOBAL_DIRECT)
			     + 3 * (algorithm == NLOPT_GLOBAL_DIRECT_L_RANDOMIZED ? 2 : (algorithm != NLOPT_GLOBAL_DIRECT))
			     + 9 * (algorithm == NLOPT_GLOBAL_DIRECT_L_RANDOMIZED ? 1 : (algorithm != NLOPT_GLOBAL_DIRECT)));
	      
	 case NLOPT_GLOBAL_ORIG_DIRECT:
	 case NLOPT_GLOBAL_ORIG_DIRECT_L: 
	      switch (direct_optimize(f_direct, &d, n, lb, ub, x, fmin,
				      maxeval, -1, 0.0, 0.0,
				      pow(xtol_rel, (double) n), -1.0,
				      stop.fmin_max, 0.0,
				      NULL, 
				      algorithm == NLOPT_GLOBAL_ORIG_DIRECT
				      ? DIRECT_ORIGINAL
				      : DIRECT_GABLONSKY)) {
		  case DIRECT_INVALID_BOUNDS:
		  case DIRECT_MAXFEVAL_TOOBIG:
		  case DIRECT_INVALID_ARGS:
		       return NLOPT_INVALID_ARGS;
		  case DIRECT_INIT_FAILED:
		  case DIRECT_SAMPLEPOINTS_FAILED:
		  case DIRECT_SAMPLE_FAILED:
		       return NLOPT_FAILURE;
		  case DIRECT_MAXFEVAL_EXCEEDED:
		  case DIRECT_MAXITER_EXCEEDED:
		       return NLOPT_MAXEVAL_REACHED;
		  case DIRECT_GLOBAL_FOUND:
		       return NLOPT_FMIN_MAX_REACHED;
		  case DIRECT_VOLTOL:
		  case DIRECT_SIGMATOL:
		       return NLOPT_XTOL_REACHED;
		  case DIRECT_OUT_OF_MEMORY:
		       return NLOPT_OUT_OF_MEMORY;
	      break;
	 }

	 case NLOPT_GLOBAL_STOGO:
	 case NLOPT_GLOBAL_STOGO_RANDOMIZED:
	      if (!stogo_minimize(n, f, f_data, x, fmin, lb, ub, &stop,
				  algorithm == NLOPT_GLOBAL_STOGO
				  ? 0 : 2*n))
		   return NLOPT_FAILURE;
	      break;

	 case NLOPT_LOCAL_SUBPLEX: {
	      int iret;
	      double *scale = (double *) malloc(sizeof(double) * n);
	      if (!scale) return NLOPT_OUT_OF_MEMORY;
	      for (i = 0; i < n; ++i) {
		   if (!my_isinf(ub[i]) && !my_isinf(lb[i]))
			scale[i] = (ub[i] - lb[i]) * 0.01;
		   else if (!my_isinf(lb[i]) && x[i] > lb[i])
			scale[i] = (x[i] - lb[i]) * 0.01;
		   else if (!my_isinf(ub[i]) && x[i] < ub[i])
			scale[i] = (ub[i] - x[i]) * 0.01;
		   else
			scale[i] = 0.01 * x[i] + 0.0001;
	      }
	      iret = subplex(f_subplex, fmin, x, n, &d, &stop, scale);
	      free(scale);
	      switch (iret) {
		  case -2: return NLOPT_INVALID_ARGS;
		  case -10: return NLOPT_MAXTIME_REACHED;
		  case -1: return NLOPT_MAXEVAL_REACHED;
		  case 0: return NLOPT_XTOL_REACHED;
		  case 1: return NLOPT_SUCCESS;
		  case 2: return NLOPT_FMIN_MAX_REACHED;
		  case 20: return NLOPT_FTOL_REACHED;
		  case -200: return NLOPT_OUT_OF_MEMORY;
		  default: return NLOPT_FAILURE; /* unknown return code */
	      }
	      break;
	 }

	 case NLOPT_LOCAL_LBFGS: {
	      int iret, *nbd = (int *) malloc(sizeof(int) * n);
	      if (!nbd) return NLOPT_OUT_OF_MEMORY;
	      for (i = 0; i < n; ++i) {
		   int linf = my_isinf(lb[i]) && lb[i] < 0;
		   int uinf = my_isinf(ub[i]) && ub[i] > 0;
		   nbd[i] = linf && uinf ? 0 : (uinf ? 1 : (linf ? 3 : 2));
	      }
	      iret = lbfgsb_minimize(n, f, f_data, x, nbd, lb, ub,
				     MIN(n, 5), 0.0, ftol_rel, 
				     xtol_abs ? *xtol_abs : xtol_rel,
				     maxeval);
	      free(nbd);
	      if (iret <= 0) {
		   switch (iret) {
		       case -1: return NLOPT_INVALID_ARGS;
		       case -2: default: return NLOPT_FAILURE;
		   }
	      }
	      else {
		   *fmin = f(n, x, NULL, f_data);
		   switch (iret) {
		       case 5: return NLOPT_MAXEVAL_REACHED;
		       case 2: return NLOPT_XTOL_REACHED;
		       case 1: return NLOPT_FTOL_REACHED;
		       default: return NLOPT_SUCCESS;
		   }
	      }
	      break;
	 }

	 default:
	      return NLOPT_INVALID_ARGS;
     }

     return NLOPT_SUCCESS;
}

nlopt_result nlopt_minimize(
     nlopt_algorithm algorithm,
     int n, nlopt_func f, void *f_data,
     const double *lb, const double *ub, /* bounds */
     double *x, /* in: initial guess, out: minimizer */
     double *fmin, /* out: minimum */
     double fmin_max, double ftol_rel, double ftol_abs,
     double xtol_rel, const double *xtol_abs,
     int maxeval, double maxtime)
{
     nlopt_result ret;
     if (xtol_abs)
	  ret = nlopt_minimize_(algorithm, n, f, f_data, lb, ub,
				x, fmin, fmin_max, ftol_rel, ftol_abs,
				xtol_rel, xtol_abs, maxeval, maxtime);
     else {
	  int i;
	  double *xtol = (double *) malloc(sizeof(double) * n);
	  if (!xtol) return NLOPT_OUT_OF_MEMORY;
	  for (i = 0; i < n; ++i) xtol[i] = -1;
	  ret = nlopt_minimize_(algorithm, n, f, f_data, lb, ub,
				x, fmin, fmin_max, ftol_rel, ftol_abs,
				xtol_rel, xtol, maxeval, maxtime);
	  free(xtol);
     }
     return ret;
}
