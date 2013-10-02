#include <math.h>
#include <htslib/kfunc.h>
#include "call.h"

// void ccall_init(call_t *call) { return; }
// void ccall_destroy(call_t *call) { return; }
// int ccall(call_t *call, bcf1_t *rec) { return 0; }

void qcall_init(call_t *call) { return; }
void qcall_destroy(call_t *call) { return; }
int qcall(call_t *call, bcf1_t *rec) 
{ 
    // QCall format: 
    //  chromosome, position, reference allele, depth, mapping quality, 0, ..
    error("TODO: qcall output\n");
    return 0; 
}

void call_init_pl2p(call_t *call)
{
    int i;
    for (i=0; i<256; i++)
        call->pl2p[i] = pow(10., -i/10.);
}

static void mcall_init_trios(call_t *call)
{
    // 23, 138, 478 possible trio genotypes with 2, 3, 4 alleles
    call->ntrio[2] = 23, call->ntrio[3] = 138, call->ntrio[4] = 478;
    call->trio[2] = (uint16_t*) malloc(sizeof(uint16_t)*call->ntrio[2]);
    call->trio[3] = (uint16_t*) malloc(sizeof(uint16_t)*call->ntrio[3]);
    call->trio[4] = (uint16_t*) malloc(sizeof(uint16_t)*call->ntrio[4]);

    // max 10 possible diploid genotypes
    int gts[10], nals;
    for (nals=2; nals<=4; nals++)
    {
        int i,j,k, n = 0, ngts = 0;
        for (i=0; i<nals; i++)
            for (j=0; j<=i; j++)
                gts[ngts++] = 1<<i | 1<<j;

        for (i=0; i<ngts; i++)
            for (j=0; j<ngts; j++)
                for (k=0; k<ngts; k++)
                {
                    if ( ((gts[i]|gts[j])&gts[k]) != gts[k] ) continue;
                    assert( n<call->ntrio[nals] );
                    call->trio[nals][n++] = i<<8 | j<<4 | k;  // father, mother, child
                }
    }
    call->GLs = (double*) malloc(sizeof(double)*bcf_nsamples(call->hdr)*10);
    call->sumGLs = (double*) malloc(sizeof(double)*bcf_nsamples(call->hdr));
    call->GQs = (int*) malloc(sizeof(int)*bcf_nsamples(call->hdr));
}
static void mcall_destroy_trios(call_t *call)
{
    int i;
    for (i=2; i<=4; i++) free(call->trio[i]);
}

void mcall_init(call_t *call) 
{ 
    call_init_pl2p(call);

    call->nqsum = 4;
    call->qsum  = (float*) malloc(sizeof(float)*call->nqsum); 
    call->nals_map = 4;
    call->als_map  = (int*) malloc(sizeof(int)*call->nals_map);
    call->npl_map  = 4*(4+1)/2;
    call->pl_map   = (int*) malloc(sizeof(int)*call->npl_map);
    call->gts = (int*) calloc(bcf_nsamples(call->hdr)*2,sizeof(int));   // assuming at most diploid everywhere

    if ( call->flag & CALL_CONSTR_TRIO ) mcall_init_trios(call);

    bcf_hdr_append(call->hdr,"##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">");
    bcf_hdr_append(call->hdr,"##FORMAT=<ID=GQ,Number=1,Type=Integer,Description=\"Genotype Quality\">");
    bcf_hdr_append(call->hdr,"##INFO=<ID=ICB,Number=1,Type=Float,Description=\"Inbreeding Coefficient Binomial test (bigger is better)\">");
    bcf_hdr_append(call->hdr,"##INFO=<ID=HOB,Number=1,Type=Float,Description=\"Bias in the number of HOMs number (smaller is better)\">");
    bcf_hdr_append(call->hdr,"##INFO=<ID=AC,Number=A,Type=Integer,Description=\"Allele count in genotypes for each ALT allele, in the same order as listed\">");
    bcf_hdr_append(call->hdr,"##INFO=<ID=AN,Number=1,Type=Integer,Description=\"Total number of alleles in called genotypes\">");
    bcf_hdr_append(call->hdr,"##INFO=<ID=DP4,Number=4,Type=Integer,Description=\"Number of high-quality ref-forward , ref-reverse, alt-forward and alt-reverse bases\">");

    return; 
}

void mcall_destroy(call_t *call) 
{ 
    free(call->itmp);
    mcall_destroy_trios(call);
    free(call->GLs);
    free(call->sumGLs);
    free(call->GQs);
    free(call->anno16);
    free(call->PLs);
    free(call->qsum);
    free(call->als_map);
    free(call->pl_map);
    free(call->gts);
    free(call->pdg);
    free(call->als);
    return; 
}


// Inits P(D|G): convert PLs from log space and normalize. In case of zero
// depth, missing PLs are all zero. In this case, pdg's are set to 0
// so that the corresponding genotypes can be set as missing and the
// qual calculation is not affected.
// NB: While the -m callig model uses the pdgs in canonical order, 
// the original samtools -c calling code uses pdgs in reverse order (AA comes
// first, RR last).
void set_pdg(double *pl2p, int *PLs, double *pdg, int n_smpl, int n_gt)
{
    int i, j;
    for (i=0; i<n_smpl; i++)
    {
        double sum = 0;
        for (j=0; j<n_gt; j++)
        {
            assert( PLs[j]<256 );
            pdg[j] = pl2p[ PLs[j] ];
            sum += pdg[j];
        }
        // Normalize: sum_i pdg_i = 1
        if ( sum!=n_gt )
            for (j=0; j<n_gt; j++) pdg[j] /= sum;
        else
            for (j=0; j<n_gt; j++) pdg[j] = 0;

        PLs += n_gt;
        pdg += n_gt;
    }
}

// Create mapping between old and new (trimmed) alleles
void init_allele_trimming_maps(call_t *call, int als, int nals)
{
    int i, j;

    // als_map: old(i) -> new(j)
    for (i=0, j=0; i<nals; i++)
    {
        if ( als & 1<<i ) call->als_map[i] = j++;
        else call->als_map[i] = -1;
    }

    // pl_map: new(k) -> old(l)
    int k = 0, l = 0;
    for (i=0; i<nals; i++)
    {
        for (j=0; j<=i; j++)
        {
            if ( (als & 1<<i) && (als & 1<<j) ) call->pl_map[k++] = l;
            l++;
        }
    }
}

double binom_dist(int N, double p, int k)
{
    int mean = (int) (N*p);
    if ( mean==k ) return 1.0;

    double log_p = (k-mean)*log(p) + (mean-k)*log(1.0-p);
    if ( k > N - k ) k = N - k;
    if ( mean > N - mean ) mean = N - mean;
    
    if ( k < mean ) { int tmp = k; k = mean; mean = tmp; }
    double diff = k - mean;

    double val = 1.0;
    int i;
    for (i=0; i<diff; i++)
        val = val * (N-mean-i) / (k-i);

    return exp(log_p)/val;
}


// Inbreeding Coefficient, binomial test
float calc_ICB(int nref, int nalt, int nhets, int ndiploid)
{
    if ( !nref || !nalt || !ndiploid ) return HUGE_VAL;

    double fref = (double)nref/(nref+nalt); // fraction of reference allelels
    double falt = (double)nalt/(nref+nalt); // non-ref als
    double q = 2*fref*falt;                 // probability of a het, assuming HWE
    double mean = q*ndiploid;

    //fprintf(stderr,"\np=%e N=%d k=%d  .. nref=%d nalt=%d nhets=%d ndiploid=%d\n", q,ndiploid,nhets, nref,nalt,nhets,ndiploid);

    // Can we use normal approximation? The second condition is for performance only
    // and is not well justified. 
    if ( (mean>10 && (1-q)*ndiploid>10 ) || ndiploid>200 )
    {
        //fprintf(stderr,"out: mean=%e  p=%e\n", mean,exp(-0.5*(nhets-mean)*(nhets-mean)/(mean*(1-q))));
        return exp(-0.5*(nhets-mean)*(nhets-mean)/(mean*(1-q)));
    }

    return binom_dist(ndiploid, q, nhets);
}

float calc_HOB(int nref, int nalt, int nhets, int ndiploid)
{
    if ( !nref || !nalt || !ndiploid ) return HUGE_VAL;

    double fref = (double)nref/(nref+nalt); // fraction of reference allelels
    double falt = (double)nalt/(nref+nalt); // non-ref als
    return fabs((double)nhets/ndiploid - 2*fref*falt);
}

/**
  *  log(sum_i exp(a_i))
  */
inline double logsumexp(double *vals, int nvals)
{
    int i;
    double max_exp = vals[0];
    for (i=1; i<nvals; i++)
        if ( max_exp < vals[i] ) max_exp = vals[i];

    double sum = 0;
    for (i=0; i<nvals; i++)
        sum += exp(vals[i] - max_exp);

    return log(sum) + max_exp;
}
inline double logsumexp2(double a, double b)
{
    if ( a>b )
        return log(1 + exp(b-a)) + a;
    else
        return log(1 + exp(a-b)) + b;
}

// Macro to set the most likely and second most likely alleles
#define UPDATE_MAX_LKs(als) { \
     if ( max_lk<lk_tot ) { max_lk2 = max_lk; max_als2 = max_als; max_lk = lk_tot; max_als = (als); } \
     else if ( max_lk2<lk_tot ) { max_lk2 = lk_tot; max_als2 = (als); } \
     if ( lk_tot_set ) lk_sum = logsumexp2(lk_tot,lk_sum); \
}

#define SWAP(type_t,x,y) {type_t tmp; tmp = x; x = y; y = tmp; }

// Determine the most likely combination of alleles. In this implementation,
// at most tri-allelic sites are considered. Returns the number of alleles.
static int mcall_find_best_alleles(call_t *call, int nals, int *out_als)
{
    int ia,ib,ic;   // iterators over up to three alleles
    int max_als=0, max_als2=0;  // most likely and second-most likely combination of alleles
    double ref_lk = 0, max_lk = INT_MIN, max_lk2 = INT_MIN; // likelihood of the reference and of most likely combination of alleles
    double lk_sum = -HUGE_VAL;    // for normalizing the likelihoods
    int nsmpl = bcf_nsamples(call->hdr);
    int ngts  = nals*(nals+1)/2;

    // Single allele
    for (ia=0; ia<nals; ia++)
    {
        double lk_tot  = 0;
        int lk_tot_set = 0;
        int iaa = (ia+1)*(ia+2)/2-1;    // index in PL which corresponds to the homozygous "ia/ia" genotype
        int isample;
        double *pdg = call->pdg + iaa;
        for (isample=0; isample<nsmpl; isample++)
        {
            if ( *pdg ) { lk_tot += log(*pdg); lk_tot_set = 1; }
            pdg += ngts;
        }
        if ( ia==0 ) ref_lk = lk_tot;   // likelihood of 0/0 for all samples
        UPDATE_MAX_LKs(1<<ia);
    }

    // Two alleles
    if ( nals>1 )
    {
        for (ia=0; ia<nals; ia++)
        {
            if ( call->qsum[ia]==0 ) continue;
            int iaa = (ia+1)*(ia+2)/2-1;
            for (ib=0; ib<ia; ib++)
            {
                if ( call->qsum[ib]==0 ) continue;
                double lk_tot  = 0;
                int lk_tot_set = 0;
                double fa  = call->qsum[ia]/(call->qsum[ia]+call->qsum[ib]);
                double fb  = call->qsum[ib]/(call->qsum[ia]+call->qsum[ib]);
                double fab = 2*fa*fb; fa *= fa; fb *= fb;
                int isample, ibb = (ib+1)*(ib+2)/2-1, iab = iaa - ia + ib;
                double *pdg  = call->pdg;
                for (isample=0; isample<nsmpl; isample++)
                {
                    double val;
                    if ( call->ploidy && call->ploidy[isample]==1 )
                        val = fa*pdg[iaa] + fb*pdg[ibb];
                    else 
                        val = fa*pdg[iaa] + fb*pdg[ibb] + fab*pdg[iab];
                    if ( val ) { lk_tot += log(val); lk_tot_set = 1; }
                    pdg += ngts;
                }
                UPDATE_MAX_LKs(1<<ia|1<<ib);
            }
        }
    }

    // Three alleles
    if ( nals>2 )
    {
        for (ia=0; ia<nals; ia++)
        {
            if ( call->qsum[ia]==0 ) continue;
            int iaa = (ia+1)*(ia+2)/2-1;
            for (ib=0; ib<ia; ib++)
            {
                if ( call->qsum[ib]==0 ) continue;
                int ibb = (ib+1)*(ib+2)/2-1; 
                int iab = iaa - ia + ib;
                for (ic=0; ic<ib; ic++)
                {
                    if ( call->qsum[ic]==0 ) continue;
                    double lk_tot  = 0;
                    int lk_tot_set = 1;
                    double fa  = call->qsum[ia]/(call->qsum[ia]+call->qsum[ib]+call->qsum[ic]);
                    double fb  = call->qsum[ib]/(call->qsum[ia]+call->qsum[ib]+call->qsum[ic]);
                    double fc  = call->qsum[ic]/(call->qsum[ia]+call->qsum[ib]+call->qsum[ic]);
                    double fab = 2*fa*fb, fac = 2*fa*fc, fbc = 2*fb*fc; fa *= fa; fb *= fb; fc *= fc;
                    int isample, icc = (ic+1)*(ic+2)/2-1;
                    int iac = iaa - ia + ic, ibc = ibb - ib + ic;
                    double *pdg = call->pdg;
                    for (isample=0; isample<nsmpl; isample++)
                    {
                        double val = 0;
                        if ( call->ploidy && call->ploidy[isample]==1 ) 
                            val = fa*pdg[iaa] + fb*pdg[ibb] + fc*pdg[icc];
                        else
                            val = fa*pdg[iaa] + fb*pdg[ibb] + fc*pdg[icc] + fab*pdg[iab] + fac*pdg[iac] + fbc*pdg[ibc];
                        if ( val ) { lk_tot += log(val); lk_tot_set = 1; }
                        pdg += ngts;
                    }
                    UPDATE_MAX_LKs(1<<ia|1<<ib|1<<ic);
                }
            }
        }
    }

    // How many alleles to call? Add a new allele only if it increases the likelihood significantly.
    // If the most likely set has more alleles than the second most likely set but the difference is
    // not big, go with fewer alleles. Here we use chi-squared distribution with 1 degree of freedom,
    // but is it correct? Is the number of degrees of freedom constant or does it depend on the
    // number of alleles? 
    int i, n1=0, n2=0;
    for (i=0; i<nals; i++) if ( max_als  & 1<<i) n1++;
    for (i=0; i<nals; i++) if ( max_als2 & 1<<i) n2++;

    //fprintf(stderr,"max_lk=%e max_lk2=%e ref_lk=%e lk_sum=%e lrt=%e (%e)  .. n1=%d n2=%d .. als1=%d als2=%d\n", max_lk,max_lk2,ref_lk,lk_sum,kf_gammap(0.5,max_lk-max_lk2),call->min_ma_lrt, n1,n2, max_als,max_als2);

    // Xi^2 = -2*ln(P0 / P1) and CDF(0.5*ndf,0.5*x)
    if ( n1>n2 && kf_gammap(0.5,max_lk-max_lk2)<call->min_ma_lrt )
    {
        // Going with fewer alleles, the likelihood is not significantly bigger
        SWAP(double, max_lk, max_lk2);
        SWAP(int, max_als, max_als2);
        SWAP(int, n1, n2);
    }

    call->ref_lk = ref_lk;
    call->lk_sum = lk_sum;
    *out_als = max_als;
    return n1;
}

static void mcall_set_ref_genotypes(call_t *call, int nals)
{
    int i;
    int ngts  = nals*(nals+1)/2;
    int nsmpl = bcf_nsamples(call->hdr);

    for (i=0; i<4; i++) call->ac[i] = 0;
    call->nhets = 0;
    call->ndiploid = 0;

    // Set all genotypes to 0/0 and remove PL vector
    int *gts    = call->gts;
    double *pdg = call->pdg;
    int isample;
    for (isample = 0; isample < nsmpl; isample++) 
    {
        int ploidy = call->ploidy ? call->ploidy[isample] : 2;
        for (i=0; i<ngts; i++) if ( pdg[i]!=0.0 ) break;
        if ( i==ngts ) 
        {
            gts[0] = bcf_gt_missing;
            gts[1] = ploidy==2 ? bcf_gt_missing : bcf_int32_vector_end;
        }
        else
        {
            gts[0] = bcf_gt_unphased(0);
            gts[1] = ploidy==2 ? bcf_gt_unphased(0) : bcf_int32_vector_end;
            call->ac[0] += ploidy;
        }
        gts += 2;
        pdg += ngts;
    }
}

static void mcall_call_genotypes(call_t *call, int nals, int nout_als, int out_als)
{
    int ia, ib, i;
    int ngts  = nals*(nals+1)/2;
    int nsmpl = bcf_nsamples(call->hdr);

    for (i=0; i<4; i++) call->ac[i] = 0;
    call->nhets = 0;
    call->ndiploid = 0;

    double *pdg  = call->pdg - ngts;
    int *gts  = call->gts - 2;

    int isample;
    for (isample = 0; isample < nsmpl; isample++) 
    {
        int ploidy = call->ploidy ? call->ploidy[isample] : 2;

        pdg += ngts;
        gts += 2;

        // Skip samples with all pdg's equal to 1. These have zero depth.
        for (i=0; i<ngts; i++) if ( pdg[i]!=0.0 ) break;
        if ( i==ngts ) 
        {
            gts[0] = bcf_gt_missing;
            gts[1] = ploidy==2 ? bcf_gt_missing : bcf_int32_vector_end;
        }
        else
        {
            if ( ploidy==2 ) call->ndiploid++;

            // Non-zero depth, determine the most likely genotype
            double best_lk = 0;
            for (ia=0; ia<nals; ia++)
            {
                if ( !(out_als & 1<<ia) ) continue;     // ia-th allele not in the final selection, skip
                int iaa = (ia+1)*(ia+2)/2-1;            // PL index of the ia/ia genotype
                double lk = pdg[iaa]*call->qsum[ia]*call->qsum[ia];
                if ( best_lk < lk ) 
                { 
                    best_lk = lk; 
                    gts[0] = bcf_gt_unphased(call->als_map[ia]); 
                }
            }
            if ( ploidy==2 ) 
            {
                gts[1] = gts[0];
                for (ia=0; ia<nals; ia++)
                {
                    if ( !(out_als & 1<<ia) ) continue;
                    int iaa = (ia+1)*(ia+2)/2-1;
                    for (ib=0; ib<ia; ib++)
                    {
                        if ( !(out_als & 1<<ib) ) continue;
                        int iab = iaa - ia + ib;
                        double lk = 2*pdg[iab]*call->qsum[ia]*call->qsum[ib];
                        if ( best_lk < lk ) 
                        { 
                            best_lk = lk; 
                            gts[0] = bcf_gt_unphased(call->als_map[ia]); 
                            gts[1] = bcf_gt_unphased(call->als_map[ib]); 
                        }
                    }
                }
                if ( gts[0] != gts[1] ) call->nhets++;
            }
            else
                gts[1] = bcf_int32_vector_end;
        }

        call->ac[ bcf_gt_allele(gts[0]) ]++;
        if ( gts[1]!=bcf_int32_vector_end ) call->ac[ bcf_gt_allele(gts[1]) ]++;
    }
}


static void mcall_call_trio_genotypes(call_t *call, int nals, int nout_als, int out_als)
{
    int ia, ib, i;
    int nsmpl   = bcf_nsamples(call->hdr);
    int ngts    = nals*(nals+1)/2;
    double *gls = call->GLs - ngts;
    double *pdg = call->pdg - ngts;

    // Collect unconstrained genotype likelihoods
    int isample;
    for (isample = 0; isample < nsmpl; isample++) 
    {
        int ploidy = call->ploidy ? call->ploidy[isample] : 2;

        gls += ngts;
        pdg += ngts;

        // Skip samples with all pdg's equal to 1. These have zero depth.
        for (i=0; i<ngts; i++) if ( pdg[i]!=0.0 ) break;
        if ( i==ngts ) { gls[0] = 1; continue; }

        double sum_lk = 0;
        for (ia=0; ia<nals; ia++)
        {
            if ( !(out_als & 1<<ia) ) continue;     // ia-th allele not in the final selection, skip
            int iaa   = (ia+1)*(ia+2)/2-1;           // PL index of the ia/ia genotype
            int idx   = (call->als_map[ia]+1)*(call->als_map[ia]+2)/2-1;
            double lk = pdg[iaa]*call->qsum[ia]*call->qsum[ia];
            sum_lk   += lk;
            gls[idx]  = log(lk);
        }
        if ( ploidy==2 ) 
        {
            for (ia=0; ia<nals; ia++)
            {
                if ( !(out_als & 1<<ia) ) continue;
                int iaa = (ia+1)*(ia+2)/2-1;
                int ida = (call->als_map[ia]+1)*(call->als_map[ia]+2)/2-1;
                for (ib=0; ib<ia; ib++)
                {
                    if ( !(out_als & 1<<ib) ) continue;
                    int iab   = iaa - ia + ib;
                    int idx   = ida - call->als_map[ia] + call->als_map[ib];
                    double lk = 2*pdg[iab]*call->qsum[ia]*call->qsum[ib];
                    sum_lk   += lk;
                    gls[idx]  = log(lk);
                }
            }
        }
        call->sumGLs[isample] = log(sum_lk);
    }

    for (i=0; i<4; i++) call->ac[i] = 0;
    call->nhets = 0;
    call->ndiploid = 0;

    int ntrio = call->ntrio[nout_als];
    uint16_t *trio = call->trio[nout_als];
    
    // Calculate constrained likelihoods and determine genotypes
    int ifm;
    for (ifm=0; ifm<call->nfams; ifm++)
    {
        family_t *fam = &call->fams[ifm];

        // Best constrained likelihood
        double cbest = -HUGE_VAL;
        int ibest = 0, itr;
        for (itr=0; itr<ntrio; itr++)   // for each trio genotype combination
        {
            double lk = 0;
            for (i=0; i<3; i++)     // for father, mother, child
            {
                int ismpl = fam->sample[i];
                double *gl = call->GLs + ngts*ismpl;
                int igt = trio[itr]>>((2-i)*4) & 0xf;
                lk += gl[igt];
            }
            if ( cbest < lk ) { cbest = lk; ibest = itr; }
        }

        // Set the genotype quality. The current way is not correct, P(igt) != P(igt|constraint)
        for (i=0; i<3; i++) 
        {
            int igt = trio[ibest]>>((2-i)*4) & 0xf;
            int ismpl = fam->sample[i];
            double *gl  = call->GLs + ngts*ismpl;
            if ( gl[0]==1 )
                call->GQs[ismpl] = bcf_int32_missing;
            else
            {
                double pval = -4.343*log(1.0 - exp(gl[igt] - call->sumGLs[ismpl]));
                call->GQs[ismpl] = pval > 99 ? 99 : pval;
                if ( call->GQs[ismpl] > 99 ) call->GQs[ismpl] = 99;
            }
        }

        // Set genotypes for father, mother, child
        for (i=0; i<3; i++)
        {
            int ismpl  = fam->sample[i];
            int ploidy = call->ploidy ? call->ploidy[ismpl] : 2;
            double *gl = call->GLs + ngts*ismpl;
            int *gts   = call->gts + 2*ismpl;
            if ( gl[0]==1 )    // zero depth, set missing genotypes
            {
                gts[0] = bcf_gt_missing;
                gts[1] = ploidy==2 ? bcf_gt_missing : bcf_int32_vector_end;
                continue;
            }
            int igt = trio[ibest]>>((2-i)*4) & 0xf;

            // Convert genotype index idx to allele indices
            int k = 0, dk = 1;
            while ( k<igt ) k += ++dk;
            int ia = dk - 1;
            int ib = igt - k + ia;
            gts[0] = bcf_gt_unphased(ib); 
            call->ac[ib]++;
            if ( ploidy==2 )
            {
                call->ac[ia]++;
                if ( ia!=ib ) call->nhets++;
                gts[1] = bcf_gt_unphased(ia);
                call->ndiploid++;
            }
            else 
                gts[1] = bcf_int32_vector_end;
        }
    }
}

static void mcall_trim_PLs(call_t *call, bcf1_t *rec, int nals, int nout_als, int out_als)
{
    int ngts  = nals*(nals+1)/2;
    int npls_src = ngts, npls_dst = nout_als*(nout_als+1)/2;     // number of PL values in diploid samples, ori and new
    if ( npls_src == npls_dst ) return;

    int *pls_src = call->PLs, *pls_dst = call->PLs;

    int nsmpl = bcf_nsamples(call->hdr);
    int isample, ia;
    for (isample = 0; isample < nsmpl; isample++) 
    {
        int ploidy = call->ploidy ? call->ploidy[isample] : 2;
        if ( ploidy==2 )
        {
            for (ia=0; ia<npls_dst; ia++)
                pls_dst[ia] =  pls_src[ call->pl_map[ia] ];
        }
        else
        {
            for (ia=0; ia<nout_als; ia++)
            {
                int isrc = call->pl_map[ia]; 
                isrc = (isrc+1)*(isrc+2)/2-1;
                pls_dst[ia] = pls_src[isrc];
            }
            if ( ia<npls_dst ) pls_dst[ia] = bcf_int32_vector_end;
        }
        pls_src += npls_src;
        pls_dst += npls_dst;
    }
    bcf1_update_format_int32(call->hdr, rec, "PL", call->PLs, npls_dst*nsmpl);
}

static void mcall_constrain_alleles(call_t *call, bcf1_t *rec)
{
    bcf_sr_regions_t *tgt = call->srs->targets;
    if ( tgt->nals>4 ) error("Maximum accepted number of alleles is 4, got %d\n", tgt->nals);
    hts_expand(char*,tgt->nals+1,call->nals,call->als);

    int has_x = rec->d.allele[rec->n_allele-1][0]=='X' ? 1 : 0;
    int has_new = 0;

    int i, j, nals = 1;
    for (i=1; i<call->nals_map; i++) call->als_map[i] = -1;

    // always keep the reference allele
    call->als[0] = rec->d.allele[0];
    call->als_map[0] = 0;

    // create mapping from new to old alleles
    for (i=0; i<tgt->nals; i++)
    {
        if ( !strcmp(tgt->als[i],rec->d.allele[0]) ) continue;   // reference allele already added

        // is this a new allele?
        for (j=0; j<rec->n_allele; j++)
            if ( !strcmp(tgt->als[i],rec->d.allele[j]) ) break;

        if ( j<tgt->nals ) // existing allele
        {
            call->als[nals] = rec->d.allele[j];
            call->als_map[nals] = j;
        }
        else // new allele
        {
            has_new = 1;
            call->als[nals] = tgt->als[i];
            if ( has_x==1 ) call->als_map[nals] = rec->n_allele - 1;
            else has_x = -1;
        }
        nals++;
    }
    if ( !has_new ) return;
    assert( has_x != -1 );  // todo
    bcf1_update_alleles(call->hdr, rec, (const char**)call->als, nals);
    
    // create mapping from new PL to old PL
    int k = 0;
    for (i=0; i<nals; i++)
    {
        for (j=0; j<=i; j++)
            call->pl_map[k++] = call->als_map[i]*(call->als_map[i]+1)/2 + call->als_map[j];
    }

    // update PL
    call->nPLs = bcf_get_format_int(call->hdr, rec, "PL", &call->PLs, &call->mPLs);
    int nsmpl  = bcf_nsamples(call->hdr);
    int npls_ori = call->nPLs / nsmpl;
    int npls_new = k;
    hts_expand(int,npls_new*nsmpl,call->n_itmp,call->itmp);
    int *ori = call->PLs, *new = call->itmp;
    for (i=0; i<nsmpl; i++)
    {
        for (k=0; k<npls_new; k++) new[k] = ori[call->pl_map[k]];
        ori += npls_ori;
        new += npls_new;
    }
    bcf1_update_format_int32(call->hdr, rec, "PL", call->itmp, npls_new*nsmpl);

    // update QS
    float qsum[4];
    int nqs = bcf_get_info_float(call->hdr, rec, "QS", &call->qsum, &call->nqsum);
    for (i=0; i<nals; i++)
        qsum[i] = i<nqs ? call->qsum[call->als_map[i]] : 0;
    bcf1_update_info_float(call->hdr, rec, "QS", qsum, nals);
}


/**
  *  This function implements the multiallelic calling model. It has two major parts:
  *   1) determine the most likely set of alleles and calculate the quality of ref/non-ref site
  *   2) determine and set the genotypes
  *  In various places in between, the BCF record gets updated.
  */
int mcall(call_t *call, bcf1_t *rec)
{
    // Force alleles when calling genotypes given alleles was requested
    if ( call->flag & CALL_CONSTR_ALLELES ) mcall_constrain_alleles(call, rec);

    int nsmpl = bcf_nsamples(call->hdr);
    int nals  = rec->n_allele;
    if ( nals>4 )
        error("FIXME: Not ready for more than 4 alleles at %s:%d (%d)\n", call->hdr->id[BCF_DT_CTG][rec->rid].key,rec->pos+1, nals);

    // Get the genotype likelihoods
    call->nPLs = bcf_get_format_int(call->hdr, rec, "PL", &call->PLs, &call->mPLs);
    if ( call->nPLs!=nsmpl*nals*(nals+1)/2 && call->nPLs!=nsmpl*nals )  // a mixture of diploid and haploid or haploid only
        error("Wrong number of PL fields? nals=%d npl=%d\n", nals,call->nPLs);

    // Convert PLs to probabilities
    int ngts = nals*(nals+1)/2;
    hts_expand(double, call->nPLs, call->npdg, call->pdg);
    set_pdg(call->pl2p, call->PLs, call->pdg, nsmpl, ngts);

    // Get sum of qualities
    int i, nqs = bcf_get_info_float(call->hdr, rec, "QS", &call->qsum, &call->nqsum);
    assert( nals<=call->nqsum );
    for (i=nqs; i<nals; i++) call->qsum[i] = 0;

    // Find the best combination of alleles
    int out_als, nout =  mcall_find_best_alleles(call, nals, &out_als);

    // With -A, keep all ALTs except X
    if ( call->flag & CALL_KEEPALT )
    {
        nout = 0;
        for (i=0; i<nals; i++)
            if ( rec->d.allele[i][0]!='X' ) { out_als |= 1<<i; nout++; }
    }
    // Make sure the REF allele is always present
    else if ( !(out_als&1) )
    {
        out_als |= 1;
        nout++;
    }

    if ( call->flag & CALL_VARONLY && out_als==1 ) return 0;
    int nAC = 0;
    if ( out_als==1 )
    { 
        init_allele_trimming_maps(call, 1, 1);
        mcall_set_ref_genotypes(call,nals);
        bcf1_update_format_int32(call->hdr, rec, "PL", NULL, 0);    // remove PL, useless now
    }
    else
    {
        // The most likely set of alleles includes non-reference allele (or was enforced), call genotypes.
        // Note that it is a valid outcome if the called genotypes exclude some of the ALTs.
        init_allele_trimming_maps(call, out_als, nals);
        if ( call->flag & CALL_CONSTR_TRIO )
        {
            mcall_call_trio_genotypes(call,nals,nout,out_als);
            bcf1_update_format_int32(call->hdr, rec, "GQ", call->GQs, nsmpl);
        }
        else
            mcall_call_genotypes(call,nals,nout,out_als);

        // Skip the site if all samples are 0/0. This can happen occasionally.
        nAC = call->ac[1] + call->ac[2] + call->ac[3];
        if ( !nAC && call->flag & CALL_VARONLY ) return 0;
        mcall_trim_PLs(call, rec, nals, nout, out_als);
    }

    // Set QUAL and calculate HWE-related annotations
    if ( nAC ) 
    {
        float icb = calc_ICB(call->ac[0],nAC, call->nhets, call->ndiploid);
        if ( icb != HUGE_VAL ) bcf1_update_info_float(call->hdr, rec, "ICB", &icb, 1);

        float hob = calc_HOB(call->ac[0],nAC, call->nhets, call->ndiploid);
        if ( hob != HUGE_VAL ) bcf1_update_info_float(call->hdr, rec, "HOB", &hob, 1);

        // Quality of a variant site
        rec->qual = call->lk_sum==-HUGE_VAL ? 0 : -4.343*(call->ref_lk - call->lk_sum);
    }
    else
    {
        // Set the quality of a REF site
        rec->qual = call->lk_sum==-HUGE_VAL ? 0 : -4.343*log(1 - exp(call->ref_lk - call->lk_sum));
    }
    if ( rec->qual>999 ) rec->qual = 999;
    if ( rec->qual>50 ) rec->qual = rint(rec->qual);

    // AC, AN
    if ( nout>1 ) bcf1_update_info_int32(call->hdr, rec, "AC", call->ac+1, nout-1);
    nAC += call->ac[0];
    bcf1_update_info_int32(call->hdr, rec, "AN", &nAC, 1);

    // Remove unused alleles
    hts_expand(char*,nout,call->nals,call->als);
    for (i=0; i<nals; i++)
        if ( call->als_map[i]>=0 ) call->als[call->als_map[i]] = rec->d.allele[i];  
    bcf1_update_alleles(call->hdr, rec, (const char**)call->als, nout);
    bcf1_update_genotypes(call->hdr, rec, call->gts, nsmpl*2);

    // DP4 tag
    if ( bcf_get_info_float(call->hdr, rec, "I16", &call->anno16, &call->n16)!=16 ) 
        error("I16 hasn't 16 fields at %s:%d\n", call->hdr->id[BCF_DT_CTG][rec->rid].key,rec->pos+1);
    int32_t dp[4]; dp[0] = call->anno16[0]; dp[1] = call->anno16[1]; dp[2] = call->anno16[2]; dp[3] = call->anno16[3];
    bcf1_update_info_int32(call->hdr, rec, "DP4", dp, 4);

    bcf1_update_info_int32(call->hdr, rec, "I16", NULL, 0);     // remove I16 tag
    bcf1_update_info_int32(call->hdr, rec, "QS", NULL, 0);      // remove QS tag

    return nout;
}

