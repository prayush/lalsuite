/*
*  Copyright (C) 2008 P. Ajith, Lucia Santamaria
*
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with with program; see the file COPYING. If not, write to the
*  Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
*  MA  02111-1307  USA
*/


#include <lal/Units.h>
#include <lal/LALInspiral.h>
#include <lal/FindRoot.h>
#include <lal/SeqFactories.h>
#include <lal/NRWaveInject.h>
#include <lal/RealFFT.h>
#include <lal/TimeFreqFFT.h>
#include <lal/SeqFactories.h>
#include <lal/VectorOps.h>
#include <lal/BBHPhenomCoeffs.h> 
#include <lal/AVFactories.h> 

#include <math.h>

typedef struct tagBBHPhenomParams{
  REAL8 fMerger;
  REAL8 fRing;
  REAL8 fCut;
  REAL8 sigma;
  REAL8 psi0;
  REAL8 psi1;
  REAL8 psi2;
  REAL8 psi3;
  REAL8 psi4;
  REAL8 psi5;
  REAL8 psi6;
  REAL8 psi7;
}
BBHPhenomParams;


static void XLALComputePhenomParams( BBHPhenomParams *phenParams,
			      InspiralTemplate *params);
 
static REAL8 XLALLorentzianFn ( REAL8 freq,
			 REAL8 fRing,
			 REAL8 sigma);


static void XLALBBHPhenWaveFD ( BBHPhenomParams  *params,
			 InspiralTemplate *template, 
			 REAL4Vector *signal);


static void XLALComputeInstantFreq( REAL4Vector *Freq,
			     REAL4Vector *hp,
			     REAL4Vector *hc,
			     REAL8 dt);

static REAL4Vector *XLALCutAtFreq( REAL4Vector     *h, 
				       REAL4Vector     *freq, 
				       REAL8           cutFreq, 
				       REAL8           deltaT);

NRCSID (LALPHENOMWAVEFORMC, "$Id$");


/*********************************************************************/
/*** Top level function to generate the phenomenological waveform ****/
/*********************************************************************/
void LALBBHPhenWaveFreqDom ( LALStatus        *status,
			     REAL4Vector      *signal,
			     InspiralTemplate *params) 
{    

  BBHPhenomParams phenParams;

  /* check inputs */
  INITSTATUS (status, "LALBBHPhenWaveFreqDom", LALPHENOMWAVEFORMC);
  ATTATCHSTATUSPTR(status);
  ASSERT (signal,  status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT (signal->data,  status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT (params, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT (signal->length>2,  status, LALINSPIRALH_ECHOICE, LALINSPIRALH_MSGECHOICE);
  
    /* compute the phenomenological parameters */
    XLALComputePhenomParams(&phenParams, params);

    /* generate the phenomenological waveform in frequency domain */
    XLALBBHPhenWaveFD (&phenParams, params, signal);

/*     params->fFinal = params->fCut; */

    DETATCHSTATUSPTR(status);
    RETURN(status);
}


static void XLALComputePhenomParams( BBHPhenomParams  *phenParams,
			      InspiralTemplate *params) 

{ 

  REAL8 totalMass, piM, eta, fMerg_a, fMerg_b, fMerg_c, fRing_a, fRing_b;
  REAL8 fRing_c, sigma_a, sigma_b, sigma_c, fCut_a, fCut_b, fCut_c;
  REAL8 psi0_a, psi0_b, psi0_c, psi2_a, psi2_b, psi2_c, psi3_a, psi3_b, psi3_c;
  REAL8 psi4_a, psi4_b, psi4_c, psi6_a, psi6_b, psi6_c, psi7_a, psi7_b, psi7_c; 
  
  /* calculate the total mass and symmetric mass ratio */

  if (params) {

    totalMass = params->mass1+params->mass2;
    eta = params->mass1*params->mass2/pow(totalMass,2.);
    piM = totalMass*LAL_PI*LAL_MTSUN_SI;
  } 
  else {
    return;
  }

  /* Select the polynomial coefficients - tuned for JenaLongUMV2 waveforms.
   * Ref. Table I of P. Ajith, arXiv:0712.0343 [gr-qc] */
  /*   fMerg_a = 6.6389e-01; fMerg_b = -1.0321e-01; fMerg_c = 1.0979e-01; */
  /*   fRing_a = 1.3278e+00; fRing_b = -2.0642e-01; fRing_c = 2.1957e-01; */
  /*   sigma_a = 1.1383e+00; sigma_b = -1.7700e-01; sigma_c = 4.6834e-02; */
  /*   fCut_a = 1.7086e+00; fCut_b = -2.6592e-01; fCut_c = 2.8236e-01; */
  
  
  /*   psi0_a = -1.5829e-01; psi0_b = 8.7016e-02; psi0_c = -3.3382e-02; */
  /*   psi2_a = 3.2967e+01; psi2_b = -1.9000e+01; psi2_c = 2.1345e+00; */
  /*   psi3_a = -3.0849e+02; psi3_b = 1.8211e+02; psi3_c = -2.1727e+01; */
  /*   psi4_a = 1.1525e+03; psi4_b = -7.1477e+02; psi4_c = 9.9692e+01; */
  /*   psi6_a = 1.2057e+03; psi6_b = -8.4233e+02; psi6_c = 1.8046e+02; */
  /*   psi7_a = -0.0000e+00; psi7_b = 0.0000e+00; psi7_c = 0.0000e+00; */


  fMerg_a = BBHPHENOMCOEFFSH_FMERG_A;
  fMerg_b = BBHPHENOMCOEFFSH_FMERG_B;
  fMerg_c = BBHPHENOMCOEFFSH_FMERG_C;  

  fRing_a = BBHPHENOMCOEFFSH_FRING_A;
  fRing_b = BBHPHENOMCOEFFSH_FRING_B;
  fRing_c = BBHPHENOMCOEFFSH_FRING_C;  

  sigma_a = BBHPHENOMCOEFFSH_SIGMA_A;
  sigma_b = BBHPHENOMCOEFFSH_SIGMA_B;
  sigma_c = BBHPHENOMCOEFFSH_SIGMA_C;  

  fCut_a = BBHPHENOMCOEFFSH_FCUT_A;
  fCut_b = BBHPHENOMCOEFFSH_FCUT_B;
  fCut_c = BBHPHENOMCOEFFSH_FCUT_C;  

  psi0_a = BBHPHENOMCOEFFSH_PSI0_X;
  psi0_b = BBHPHENOMCOEFFSH_PSI0_Y;
  psi0_c = BBHPHENOMCOEFFSH_PSI0_Z;

  psi2_a = BBHPHENOMCOEFFSH_PSI2_X;
  psi2_b = BBHPHENOMCOEFFSH_PSI2_Y;
  psi2_c = BBHPHENOMCOEFFSH_PSI2_Z;

  psi3_a = BBHPHENOMCOEFFSH_PSI3_X;
  psi3_b = BBHPHENOMCOEFFSH_PSI3_Y;
  psi3_c = BBHPHENOMCOEFFSH_PSI3_Z;

  psi4_a = BBHPHENOMCOEFFSH_PSI4_X;
  psi4_b = BBHPHENOMCOEFFSH_PSI4_Y;
  psi4_c = BBHPHENOMCOEFFSH_PSI4_Z;

  psi6_a = BBHPHENOMCOEFFSH_PSI6_X;
  psi6_b = BBHPHENOMCOEFFSH_PSI6_Y;
  psi6_c = BBHPHENOMCOEFFSH_PSI6_Z;

  psi7_a = BBHPHENOMCOEFFSH_PSI7_X;
  psi7_b = BBHPHENOMCOEFFSH_PSI7_Y;
  psi7_c = BBHPHENOMCOEFFSH_PSI7_Z;

  /* Evaluate the polynomials. See Eq. (4.18) of P. Ajith et al 
   * arXiv:0710.2335 [gr-qc] */
  if (phenParams) {

    phenParams->fCut  = (fCut_a*eta*eta  + fCut_b*eta  + fCut_c)/piM;
    phenParams->fMerger  = (fMerg_a*eta*eta  + fMerg_b*eta  + fMerg_c)/piM;
    phenParams->fRing  = (fRing_a*eta*eta + fRing_b*eta + fRing_c)/piM;
    phenParams->sigma = (sigma_a*eta*eta + sigma_b*eta + sigma_c)/piM;
    
    phenParams->psi0 = (psi0_a*eta*eta + psi0_b*eta + psi0_c)/(eta*pow(piM, 5./3.));
    phenParams->psi1 = 0.;
    phenParams->psi2 = (psi2_a*eta*eta + psi2_b*eta + psi2_c)/(eta*pow(piM, 3./3.));
    phenParams->psi3 = (psi3_a*eta*eta + psi3_b*eta + psi3_c)/(eta*pow(piM, 2./3.));
    phenParams->psi4 = (psi4_a*eta*eta + psi4_b*eta + psi4_c)/(eta*pow(piM, 1./3.));
    phenParams->psi5 = 0.;
    phenParams->psi6 = (psi6_a*eta*eta + psi6_b*eta + psi6_c)/(eta*pow(piM, -1./3.));
    phenParams->psi7 = (psi7_a*eta*eta + psi7_b*eta + psi7_c)/(eta*pow(piM, -2./3.));
  }
  
  return;
  
}


static void XLALBBHPhenWaveFD ( BBHPhenomParams  *params,
				InspiralTemplate *template, 
				REAL4Vector *signal) {

    REAL8 df, shft, phi, amp0, ampEff, psiEff, fMerg, fNorm;
    REAL8 f, fRing, sigma, totalMass, eta;
    INT4 i, j, n;

    /* freq resolution and the low-freq bin */
    df = template->tSampling/signal->length;
    n = signal->length;

    /* If we want to pad with zeroes in the beginning then the instant of
    * coalescence will be the chirp time + the duration for which padding
    * is needed. Thus, in the equation below nStartPad occurs with a +ve sign.
    * This code doesn't support non-zero start-time. i.e. params->startTime
    * should be necessarily zero.*/
    shft = 2.*LAL_PI * ((REAL4)signal->length/template->tSampling + 
            template->nStartPad/template->tSampling + template->startTime);
    phi  = template->startPhase;

    /* phenomenological  parameters*/
    fMerg = params->fMerger;
    fRing = params->fRing;
    sigma = params->sigma;
    totalMass = template->mass1 + template->mass2;
    eta = template->mass1 * template->mass2 / pow(totalMass, 2.);

    /* Now compute the amplitude.  NOTE the params->distance is assumed to 
     * me in meters. This is, in principle, inconsistent with the LAL 
     * documentation (inspiral package). But this seems to be the convention
     * employed in the injection codes */ 
    amp0 = pow(LAL_MTSUN_SI*totalMass, 5./6.)*pow(fMerg,-7./6.)/pow(LAL_PI,2./3.);
    amp0 *= pow(5.*eta/24., 1./2.)/(template->distance/LAL_C_SI);  
    amp0 *= 4.*sqrt(5./(64.*LAL_PI));

    /* fill the zero and Nyquist frequency with zeros */
    *(signal->data+0) = 0.;
    *(signal->data+n/2) = 0.;

    /* now generate the waveform at all frequency bins */
    for (i=1; i<n/2; i++) {

        /* this is the index of the imaginary part */
        j = n-i;

        /* fourier frequency corresponding to this bin */
      	f = i * df;
        fNorm = f/fMerg;

    	/* compute the amplitude */
        if ((f < template->fLower) || (f > params->fCut)) {
            ampEff = 0.;
        }
        else if (f <= fMerg) {
            ampEff = amp0*pow(fNorm, -7./6.);
        }
        else if ((f > fMerg) & (f <= fRing)) {
            ampEff = amp0*pow(fNorm, -2./3.);
        }
        else if (f > fRing) {
            ampEff = XLALLorentzianFn ( f, fRing, sigma);
            ampEff *= amp0*LAL_PI_2*pow(fRing/fMerg,-2./3.)*sigma;
        }
        
        /* now compute the phase */
       	psiEff = shft*f + phi 
                    + params->psi0*pow(f,-5./3.) 
                    + params->psi1*pow(f,-4./3.) 
                    + params->psi2*pow(f,-3./3.) 
                    + params->psi3*pow(f,-2./3.) 
                    + params->psi4*pow(f,-1./3.)
                    + params->psi5*pow(f,0.)
                    + params->psi6*pow(f,1./3.)
                    + params->psi7*pow(f,2./3.);
              
       	/* generate the waveform */
       	*(signal->data+i) = (REAL4) (ampEff * cos(psiEff));     /* real */
        *(signal->data+j) = (REAL4) (ampEff * sin(psiEff));    /* imag */
    }    

}



static REAL8 XLALLorentzianFn ( REAL8 freq,
			 REAL8 fRing,
			 REAL8 sigma) 
{
  REAL8 out;
  
  out = sigma / (2 * LAL_PI * ((freq - fRing)*(freq - fRing) 
			       + sigma*sigma / 4.0));
  
  return(out);
}


void LALBBHPhenWaveFreqDomTemplates( LALStatus        *status,
				     REAL4Vector      *signal1,
				     REAL4Vector      *signal2,
				     InspiralTemplate *params) 
{
  
  INITSTATUS(status, "LALBBHPhenWaveFreqDomTemplates", LALPHENOMWAVEFORMC);
  ATTATCHSTATUSPTR(status);
  
  ASSERT(signal1, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT(signal2, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT(signal1->data, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT(signal2->data, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  
  /* Initially the waveforms are empty */
  memset(signal1->data, 0, signal1->length * sizeof(REAL4));
  memset(signal2->data, 0, signal2->length * sizeof(REAL4));
  
  /* generate one waveform with startPhase specified by the user */
  LALBBHPhenWaveFreqDom(status->statusPtr, signal1, params);
  CHECKSTATUSPTR(status);	 
  
  /* generate another waveform orthogonal to it */
  params->startPhase += LAL_PI_2;
  LALBBHPhenWaveFreqDom(status->statusPtr, signal2, params);
  CHECKSTATUSPTR(status);	 
  
  DETATCHSTATUSPTR(status);
  RETURN (status);
  
}


void LALBBHPhenWaveTimeDom ( LALStatus        *status,
			     REAL4Vector      *signal,
			     InspiralTemplate *template) 
{

  REAL8 sharpNessLow, sharpNessUpp, fLower;
  REAL8 fCut, fRes, f, totalMass, softWin;
  REAL8 fLowerOrig, eta, tau0;
  REAL8 winFLo, winFHi, sigLo, sigHi;
  REAL4 windowLength;
  INT4 i, k, n;
  /* INT4 kMin, kMax; */
  /* REAL8 dfM;*/
  REAL4Vector *signalFD1 = NULL;
  REAL4FFTPlan *revPlan = NULL;
  /* FILE *filePtr; */
  BBHPhenomParams phenParams;
  
  /* check inputs */
  INITSTATUS (status, "LALBBHPhenWaveTimeDom", LALPHENOMWAVEFORMC);
  ATTATCHSTATUSPTR(status);
  ASSERT (signal,  status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT (signal->data,  status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT (template, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT (signal->length>2,  status, LALINSPIRALH_ECHOICE, LALINSPIRALH_MSGECHOICE);
  ASSERT (template->nStartPad >= 0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
  ASSERT (template->nEndPad >= 0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
  ASSERT (template->fLower > 0.0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
  ASSERT (template->tSampling > 0.0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
  
  
  /* compute the phenomenological parameters */
  XLALComputePhenomParams(&phenParams, template);

  totalMass = template->mass1 + template->mass2;
  eta = template->mass1*template->mass2/pow(totalMass,2.);

  /* we will generate the waveform from a frequency which is lower than the
   * fLower chosen. Also the cutoff frequency is higher than the fCut. We 
   * will later apply a window function, and truncate the time-domain waveform 
   * below an instantaneous frequency  fLower */
  fLowerOrig = template->fLower;    /* this is the low-freq set by the user */
  
  /* this is the frequency at which the softening fn has value 0.5 */
  tau0 = 32.;
/*   fLower = pow((tau0*256.*eta*pow(totalMass*LAL_MTSUN_SI,5./3.)/5.),-3./8.)/LAL_PI;  */
  /* Better ansatz (Lucia Oct 08) */
/*   fLower = 2.E-3/(totalMass*LAL_MTSUN_SI); */
  fLower = 18. - 3.*totalMass/25.;
  fCut = (1.025)*phenParams.fCut;
  
  /* make sure that these frequencies are not too out of range */
  if (fLower > fLowerOrig) fLower = fLowerOrig;
  if (fCut > template->tSampling/2.-100.) fCut = template->tSampling/2.-100.;
  
  /* generate waveforms over this frequency range */
  template->fLower = fLower;
  phenParams.fCut = template->tSampling/2.; 
  
  /* make sure that fLower is not too low */
   if (template->fLower < 0.5) template->fLower = 0.5;
  
  /* generate the phenomenological waveform in frequency domain */
  n = signal->length;
  signalFD1 = XLALCreateREAL4Vector(n);
  XLALBBHPhenWaveFD (&phenParams, template, signalFD1);
  
  /* apply the softening window function */
  fRes = template->tSampling/n;
  
  /********************************* DEBUG ********************************/
  /*      filePtr = fopen("FreqDomPhenWave.txt","a");
	  for (i = 1; i < n/2; i++) {
	  fprintf(filePtr,"%e\t%e\t%e\n", i*fRes, signalFD1->data[i],signalFD1->data[n-i]);
	  }
	  fclose(filePtr); */
  /************************************************************************/
  
  /* We will apply a window function to soften the boundaries. The function has 
     value 1e-15 at fLow-df and fCut+df*/
/*   sharpNessLow = (-1./(fLower/4.)) * atanh(1e-15 - 1); */
/*   sharpNessUpp = (-1./fCut) * atanh(1e-15 - 1); */
  
/*   softWin = (1+tanh(sharpNessLow*(0.0-fLower)))*(1-tanh(sharpNessUpp*(0.0-fCut)))/4.; */
/*   signalFD1->data[0] *= softWin; */
/*   for (k = 1; k <= n/2; k++) { */
/*     f = k*fRes; */
/*     softWin = (1+tanh(4.*sharpNessLow*(f-fLower)))*(1-tanh(4.*sharpNessUpp*(f-fCut)))/4.; */
/*     signalFD1->data[k] *= softWin; */
/*     signalFD1->data[n-k] *= softWin; */
/*     } */

  winFLo = (fLowerOrig + fLower)/2.;
  winFHi = (fCut + phenParams.fCut)/2.;
  sigLo = 4.;
  sigHi = 4.;
  
  signalFD1->data[0] *= 0.;
  for (k = 1; k <= n/2; k++) {
    f = k*fRes;
    softWin = (1+tanh((4.*(f-winFLo)/sigLo)))*(1-tanh(4.*(f-fCut)/sigHi))/4.;
    signalFD1->data[k] *= softWin;
    signalFD1->data[n-k] *= softWin;
    }

  
  /********************************* DEBUG ********************************/
  /*       filePtr = fopen("FreqDomPhenWave_Wind.txt","a");
	   for (i = 1; i < n/2; i++) {
	   fprintf(filePtr,"%e\t%e\t%e\n", i*fRes, signalFD1->data[i],signalFD1->data[n-i]);
	   }
	   fclose(filePtr); */
  /************************************************************************/
  
  /* Inverse Fourier transform */
  LALCreateReverseREAL4FFTPlan(status->statusPtr, &revPlan, n, 0 );
  LALREAL4VectorFFT(status->statusPtr, signal, signalFD1, revPlan);
  XLALDestroyREAL4Vector(signalFD1);
  LALDestroyREAL4FFTPlan(status->statusPtr, &revPlan); 
  
  /* FFT normalisation. The LAL implementation of the FFT omits the factor 1/n. 
   * Also we change the sign of the waveform so that the initialPhase = 0 and pi/2
   * will match to the 'plus' and 'cross' polarisations of the hybrid waveforms, 
     * respectively*/
  for (i = 0; i < n; i++) {
    signal->data[i] *= -template->tSampling/n;
  }
  
  /********************************* DEBUG ********************************/
  /*filePtr = fopen("TimeDomPhenWave.txt","a");
    for (i = 0; i < n; i++) {
    fprintf(filePtr,"%e\t%e\n", i/template->tSampling, signal->data[i]);
    }
    fclose(filePtr);*/
  /************************************************************************/
  
  /* apply a linearly increasing/decresing window at the beginning and at the end 
   * of the waveform in order to avoid edge effects. This could be made fancier */
/*   windowLength = 10.*totalMass * LAL_MTSUN_SI*template->tSampling; */
/*   for (i=0; i< windowLength; i++){ */
/*         signal->data[n-i-1] *= i/windowLength; */
/*   } */
/*   windowLength = 1000.*totalMass * LAL_MTSUN_SI*template->tSampling; */
/*   for (i=0; i< windowLength; i++){ */
/*     signal->data[i] *= i/windowLength; */
/*   } */
  
  /********************************* DEBUG ********************************/
  /* CHAR fileName[1000];
     sprintf(fileName, "TimeDomPhenWave_Wind_Phi0%4.3f.txt",template->startPhase);
     filePtr = fopen(fileName,"a");
     for (i = 0; i < n; i++) {
     fprintf(filePtr,"%e\t%e\n", i/template->tSampling, signal->data[i]);
     }
     fclose(filePtr); */
  /************************************************************************/
  
    /* reassign the original value of fLower */
  template->fLower = fLowerOrig;
  template->fFinal = phenParams.fCut;
  
  DETATCHSTATUSPTR(status);
  RETURN(status);
}


void LALBBHPhenWaveTimeDomTemplates( LALStatus        *status,
				     REAL4Vector      *signal1,
				     REAL4Vector      *signal2,
				     InspiralTemplate *params) 
{

  INITSTATUS(status, "LALBBHPhenWaveTimeDomTemplates", LALPHENOMWAVEFORMC);
  ATTATCHSTATUSPTR(status);
  
  ASSERT(signal1, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT(signal2, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT(signal1->data, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT(signal2->data, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  
  /* Initially the waveforms are empty */
  memset(signal1->data, 0, signal1->length * sizeof(REAL4));
  memset(signal2->data, 0, signal2->length * sizeof(REAL4));
  
  /* generate one waveform with startPhase specified by the user */
  LALBBHPhenWaveTimeDom(status->statusPtr, signal1, params);
  CHECKSTATUSPTR(status);	 
  
  /* generate another waveform orthogonal to it */
  params->startPhase += LAL_PI_2;
  LALBBHPhenWaveTimeDom(status->statusPtr, signal2, params);
  CHECKSTATUSPTR(status);	 
  
  DETATCHSTATUSPTR(status);
  RETURN (status);

}




void LALBBHPhenTimeDomEngine( LALStatus        *status,
			      REAL4Vector      *signal1,
			      REAL4Vector      *signal2,
			      REAL4Vector      *h,
			      REAL4Vector      *a,
			      REAL4Vector      *f,
			      REAL8Vector      *phiOut,
			      InspiralTemplate *params)
{

    INT4 i, j, k, n, peakAmpIdx;
    REAL8 dt, peakAmp;
    REAL8Vector *phi=NULL;
    
    INITSTATUS(status, "LALBBHPhenTimeDomEngine", LALPHENOMWAVEFORMC);
    ATTATCHSTATUSPTR(status);
    
    /* check inputs */
    ASSERT (params,  status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
    ASSERT (params->nStartPad >= 0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
    ASSERT (params->nEndPad >= 0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
    ASSERT (params->fLower > 0.0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
    ASSERT (params->tSampling > 0.0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);

    ASSERT(signal1, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
    ASSERT(signal2, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
    ASSERT(signal1->data, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
    ASSERT(signal2->data, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);

    dt = 1./params->tSampling;

    /* generate two orthogonal waveforms */
    LALBBHPhenWaveTimeDomTemplates(status->statusPtr, signal1, signal2, params);
    CHECKSTATUSPTR(status);	 

    /* compute the instantaneous frequency */
    if (f) XLALComputeInstantFreq(f, signal1, signal2, dt); 

    /* cut the waveform at the low freq given by */
    signal1 = XLALCutAtFreq( signal1, f, params->fLower, dt);
    signal2 = XLALCutAtFreq( signal2, f, params->fLower, dt);

    /* allocate memory for the temporary phase vector */
    n = signal1->length;
    if (phiOut) phi = XLALCreateREAL8Vector(n);

    /* compute the amplitude, phase and frequency.  Fill the polarisation vector h 
     * in the prescribed format */
    for (i=0; i<n; i++){
        j = 2*i;
        k = j+1;

        if (phiOut) {
            phi->data[i] = -atan2(signal2->data[i], signal1->data[i]);
        }
        
        /* fill the amplitude vector, if required. Currently we assume that both
         * polarisations are of equal ampliude, which is defined as [hp^2+hc^2]^0.5 */
        if (a) {
            a->data[j] = sqrt(pow(signal1->data[i],2.) + pow(signal2->data[i],2.));
            a->data[k] = sqrt(pow(signal1->data[i],2.) + pow(signal2->data[i],2.));
        
            /* find the peak amplitude*/
            if (a->data[i] > peakAmp) {
                peakAmp = a->data[i];
                peakAmpIdx = i;
            }
        }

        /* fill in the h vector, if required */
        if (h) {
            h->data[j] = signal1->data[i];
            h->data[k] = signal2->data[i];
        }
     }

    /* unwrap the phase */
    if (phiOut) {
        LALUnwrapREAL8Angle (status->statusPtr, phiOut, phi); 
        XLALDestroyREAL8Vector(phi);
    }

    /* store some paramteters for record keeping */
    params->vFinal = 0.5;           /* this parameter has realy no meaning here*/   
    params->tC = peakAmpIdx*dt;     /* time of coalescence. defined as the time 
                                       corresponding to the peak amplitude*/

    DETATCHSTATUSPTR(status);
    RETURN (status);
}




void LALBBHPhenWaveTimeDomForInjection (LALStatus        *status,
					CoherentGW       *waveform,
					InspiralTemplate *params,
					PPNParamStruc    *ppnParams) 
{

  REAL4Vector *a=NULL;      /* amplitude  data */
  REAL4Vector *h=NULL;      /* polarization data */
  REAL4Vector *ff=NULL;     /* frequency data */
  REAL8Vector *phi=NULL;    /* phase data */

  REAL4Vector *hp=NULL;     /* signal with initial phase 0 - temporary */
  REAL4Vector *hc=NULL;     /* signal with initial phase pi/2 - temporary */

  UINT4 count, i;
  REAL8 s, phiC;            /* phase at coalescence */
  CHAR message[256];
  InspiralInit paramsInit;   
  CreateVectorSequenceIn in;    

  INITSTATUS(status, "LALBBHPhenWaveTimeDomForInjection", LALPHENOMWAVEFORMC);
  ATTATCHSTATUSPTR(status);


  /* Make sure parameter and waveform structures exist. */
  ASSERT( params, status, LALINSPIRALH_ENULL,  LALINSPIRALH_MSGENULL );
  ASSERT(waveform, status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);  

  /* check inputs */
  ASSERT (params,  status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL);
  ASSERT (params->nStartPad >= 0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
  ASSERT (params->nEndPad >= 0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
  ASSERT (params->fLower > 0.0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);
  ASSERT (params->tSampling > 0.0, status, LALINSPIRALH_ESIZE, LALINSPIRALH_MSGESIZE);

  /* Make sure waveform fields don't exist. */
  ASSERT( !( waveform->a ), status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL );
  ASSERT( !( waveform->h ), status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL );
  ASSERT( !( waveform->f ), status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL );
  ASSERT( !( waveform->phi ), status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL );
  ASSERT( !( waveform->shift ), status, LALINSPIRALH_ENULL, LALINSPIRALH_MSGENULL );

  params->ampOrder = 0;
  sprintf(message, "WARNING: Amp Order has been reset to %d", params->ampOrder);
  LALInfo(status, message);

  /* Compute some parameters*/
  LALInspiralInit(status->statusPtr, params, &paramsInit);
  CHECKSTATUSPTR(status);   

  count = paramsInit.nbins;

  if (paramsInit.nbins==0) {
      DETATCHSTATUSPTR(status);
      RETURN (status);      
  }

  /* Now we can allocate memory and vector for coherentGW structure*/     
  LALSCreateVector(status->statusPtr, &ff, paramsInit.nbins);
  CHECKSTATUSPTR(status);
  LALSCreateVector(status->statusPtr, &a, 2*paramsInit.nbins);
  CHECKSTATUSPTR(status);
  LALDCreateVector(status->statusPtr, &phi, paramsInit.nbins);
  CHECKSTATUSPTR(status);
  LALSCreateVector(status->statusPtr, &hp, paramsInit.nbins);
  CHECKSTATUSPTR(status);
  LALSCreateVector(status->statusPtr, &hc, paramsInit.nbins);
  CHECKSTATUSPTR(status);
  
  /* By default the waveform is empty */
  memset(ff->data, 0, paramsInit.nbins * sizeof(REAL4));
  memset(a->data, 0, 2 * paramsInit.nbins * sizeof(REAL4));
  memset(phi->data, 0, paramsInit.nbins * sizeof(REAL8));

  if( params->approximant == IMRPhenomA ) {
      LALSCreateVector(status->statusPtr, &h, 2*paramsInit.nbins);
      CHECKSTATUSPTR(status);
      memset(h->data,  0, 2*paramsInit.nbins * sizeof(REAL4));
  }

  /* generate two orthogonal waveforms */
  params->startPhase = ppnParams->phi;
  LALBBHPhenTimeDomEngine(status->statusPtr, hp, hc, h, a, ff, phi, params);
  
  BEGINFAIL(status) {
    LALSDestroyVector(status->statusPtr, &ff);
    CHECKSTATUSPTR(status);
    LALSDestroyVector(status->statusPtr, &a);
    CHECKSTATUSPTR(status);
    LALDDestroyVector(status->statusPtr, &phi);
    CHECKSTATUSPTR(status);
    if( params->approximant == IMRPhenomA ){
      LALSDestroyVector(status->statusPtr, &h);
      CHECKSTATUSPTR(status);
    }
  }
  ENDFAIL(status);

  /* Check an empty waveform hasn't been returned */
  for (i = 0; i < phi->length; i++) {
    if (phi->data[i] != 0.0) break;
    if (i == phi->length - 1){
      LALSDestroyVector(status->statusPtr, &ff);
      CHECKSTATUSPTR(status);
      LALSDestroyVector(status->statusPtr, &a);
      CHECKSTATUSPTR(status);
      LALDDestroyVector(status->statusPtr, &phi);
      CHECKSTATUSPTR(status);
      if( params->approximant == IMRPhenomA){
	LALSDestroyVector(status->statusPtr, &h);
	CHECKSTATUSPTR(status);
      }
      
      DETATCHSTATUSPTR( status );
      RETURN( status );
    }
  }
  
  /* print some messages */
  sprintf(message, "fFinal = %f", params->fFinal);
  LALInfo(status, message);

  s = 0.5 * phi->data[count - 1];
  sprintf(message, "cycles = %f", s/3.14159);
  LALInfo(status, message);

  /* CHECK THIS CHECK THIS $$$$$$$$$$$$$$$$$$$$$$$$$$$*/
  sprintf(message, "final coalescence phase with respet to actual data =%f ",
          (ff->data[count-1]-ff->data[count-2])/2/3.14159); 
  LALInfo(status, message);
  
  if ( (s/LAL_PI) < 2 ){
      sprintf(message, "The waveform has only %f cycles; we don't keep waveform with less than 2 cycles.", 
	      (double) s/ (double)LAL_PI );	
      LALWarning(status, message);
  }
  else {
      
      for (i=0; i<count;i++) {
	    phi->data[i] =  -phiC + phi->data[i] + ppnParams->phi;
      }
      
      /* Allocate the waveform structures. */
      if ( ( waveform->a = (REAL4TimeVectorSeries *) 
                  LALMalloc( sizeof(REAL4TimeVectorSeries) ) ) == NULL ) {
          ABORT( status, LALINSPIRALH_EMEM,
                  LALINSPIRALH_MSGEMEM );
      }
      memset( waveform->a, 0, sizeof(REAL4TimeVectorSeries) );
      if ( ( waveform->f = (REAL4TimeSeries *)
                  LALMalloc( sizeof(REAL4TimeSeries) ) ) == NULL ) {
          LALFree( waveform->a ); waveform->a = NULL;
          ABORT( status, LALINSPIRALH_EMEM,
                  LALINSPIRALH_MSGEMEM );
      }
      memset( waveform->f, 0, sizeof(REAL4TimeSeries) );
      if ( ( waveform->phi = (REAL8TimeSeries *)
                  LALMalloc( sizeof(REAL8TimeSeries) ) ) == NULL ) {
          LALFree( waveform->a ); waveform->a = NULL;
          LALFree( waveform->f ); waveform->f = NULL;
          ABORT( status, LALINSPIRALH_EMEM,
                  LALINSPIRALH_MSGEMEM );
      }
      memset( waveform->phi, 0, sizeof(REAL8TimeSeries) );
      
      in.length = (UINT4)count;
      in.vectorLength = 2;
      LALSCreateVectorSequence( status->statusPtr,
				&( waveform->a->data ), &in );
      CHECKSTATUSPTR(status);      
      LALSCreateVector( status->statusPtr,
			&( waveform->f->data ), count);
      CHECKSTATUSPTR(status);      
      LALDCreateVector( status->statusPtr,
			&( waveform->phi->data ), count );
      CHECKSTATUSPTR(status);        
      
      /* copy the frequency, amplitude and phase data to the waveform structure */
      memcpy(waveform->f->data->data , ff->data, count*(sizeof(REAL4)));
      memcpy(waveform->a->data->data , a->data, 2*count*(sizeof(REAL4)));
      memcpy(waveform->phi->data->data ,phi->data, count*(sizeof(REAL8)));
      
      /* also set other parameters in the waveform structure */
      waveform->a->deltaT = waveform->f->deltaT = waveform->phi->deltaT = 1./params->tSampling;
      waveform->a->sampleUnits = lalStrainUnit;
      waveform->f->sampleUnits = lalHertzUnit;
      waveform->phi->sampleUnits = lalDimensionlessUnit;
      waveform->position = ppnParams->position;
      waveform->psi = ppnParams->psi;
      
      /* assign names */
      LALSnprintf( waveform->a->name, 
	  	LALNameLength, "Phenom inspiral amplitudes");
      LALSnprintf( waveform->f->name,
		  LALNameLength, "Phenom inspiral frequency");
      LALSnprintf( waveform->phi->name, 
	  	LALNameLength, "Phenom inspiral phase");
      
      /* fill some output */
      ppnParams->tc     = (double)(count-1) / params->tSampling ;
      ppnParams->length = count;
      ppnParams->dfdt   = ((REAL4)(waveform->f->data->data[count-1] 
				   - waveform->f->data->data[count-2]))* ppnParams->deltaT;
      ppnParams->fStop  = params->fFinal;
      ppnParams->termCode        = GENERATEPPNINSPIRALH_EFSTOP;
      ppnParams->termDescription = GENERATEPPNINSPIRALH_MSGEFSTOP;
      ppnParams->fStart   = ppnParams->fStartIn;

      if( params->approximant == IMRPhenomA ){
	if ( ( waveform->h = (REAL4TimeVectorSeries *)
	       LALMalloc( sizeof(REAL4TimeVectorSeries) ) ) == NULL ){
	  ABORT( status, LALINSPIRALH_EMEM, LALINSPIRALH_MSGEMEM );
	}
	memset( waveform->h, 0, sizeof(REAL4TimeVectorSeries) );
	LALSCreateVectorSequence( status->statusPtr,&( waveform->h->data ), &in );
	CHECKSTATUSPTR(status);      
	memcpy(waveform->h->data->data , h->data, 2*count*(sizeof(REAL4)));
	waveform->h->deltaT = 1./params->tSampling;
	waveform->h->sampleUnits = lalStrainUnit;
	LALSnprintf( waveform->h->name, 
		     LALNameLength, "Phenom inspiral polarizations");
	LALSDestroyVector(status->statusPtr, &h);
	CHECKSTATUSPTR(status);
      }
  } /* end phase condition*/
  
  /* free memory */
  LALSDestroyVector(status->statusPtr, &ff);
  CHECKSTATUSPTR(status);
  LALSDestroyVector(status->statusPtr, &a);
  CHECKSTATUSPTR(status);
  LALDDestroyVector(status->statusPtr, &phi);
  CHECKSTATUSPTR(status);

  LALSDestroyVector(status->statusPtr, &hc);
  CHECKSTATUSPTR(status);

  LALSDestroyVector(status->statusPtr, &hp);
  CHECKSTATUSPTR(status);

/*   LALSDestroyVector(status->statusPtr, &h); */
/*   CHECKSTATUSPTR(status); */

  DETATCHSTATUSPTR(status);
  RETURN(status);
}



static void XLALComputeInstantFreq( REAL4Vector *Freq,
			     REAL4Vector *hp,
			     REAL4Vector *hc,
			     REAL8 dt) 
{
    REAL4Vector *hpDot = NULL, *hcDot = NULL;
    UINT4 k, len;
  
    len = hp->length;
  
    hpDot= XLALCreateREAL4Vector(len);
    hcDot= XLALCreateREAL4Vector(len);
  
    /* Construct the dot vectors (2nd order differencing) */
    hpDot->data[0] = 0.0;
    hpDot->data[len-1] = 0.0;
    hcDot->data[0] = 0.0;
    hcDot->data[len-1] = 0.0;
    for( k = 1; k < len-1; k++) {
        hpDot->data[k] = 1./(2.*dt)*(hp->data[k+1]-hp->data[k-1]);
        hcDot->data[k] = 1./(2.*dt)*(hc->data[k+1]-hc->data[k-1]);
    }
  
    /* Compute frequency using the fact that  */
    /*h(t) = A(t) e^(i Phi) = Re(h) + i Im(h) */
    for( k = 0; k < len; k++) {
        Freq->data[k] = -hcDot->data[k] * hp->data[k] +hpDot->data[k] * hc->data[k];
        Freq->data[k] /= LAL_TWOPI;
        Freq->data[k] /= (pow(hp->data[k],2.) + pow(hc->data[k], 2.));
    }
  
    /* free the memory allocated for the derivative vectors */
    XLALDestroyREAL4Vector(hpDot);
    XLALDestroyREAL4Vector(hcDot);

    return;

}

static REAL4Vector *XLALCutAtFreq( REAL4Vector     *h, 
				   REAL4Vector     *freq, 
				   REAL8           cutFreq, 
				   REAL8           deltaT)
{
  REAL8 dt;
  UINT4 k, k0, kMid, len;
  REAL4 currentFreq;

  len = freq->length;
  dt = deltaT;

  /* Since the boundaries of this freq vector are likely to have   */
  /* FFT crap, let's scan the freq values starting from the middle */
  kMid = len/2;
  currentFreq = freq->data[kMid];
  k = kMid;
  
  /* freq is an increasing function of time */
  /* If we are above the cutFreq we move to the left; else to the right */
  if (currentFreq > cutFreq && k > 0)
    {
      while(currentFreq > cutFreq)
	{
	  currentFreq = freq->data[k];
	  k--;
	}
      k0 = k;
    }
  else 
    {
      while(currentFreq < cutFreq && k < len)
	{
	  currentFreq = freq->data[k];
	  k++;
	}
      k0 = k;
    }

  /* Allocate memory for the frequency series */

  for(k = 0; k < k0; k++)
    {
      h->data[k] = 0.0; 
    }

  return h;

}
