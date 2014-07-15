#include "mrilib.h"
#include "thd_conformist.c"

/*---------------------------------------------------------------------------*/
/* Features to ruminate about:
    - NN interpolation of data, for matching label datasets      [not hard]

    - allow user-input weight volume                             [DONE]

    - plusminus mapping                                          [DONE]
      with Src(x-w(x)) = Bas(x+w(x)) instead of Src(x+w(x))=B(x)

    - initial 3dAllineate phase (-allineate or -resample)        [DONE]

    - symmetric mapping,                                         [very hard]
      with Src(W(x)) = Bas(INV(W(x))) instead of Src(W(x))=B(x)

    - vector-valued images                                       [medium]

    - GPU acceleration                                           [ugh]
*//*-------------------------------------------------------------------------*/

#ifdef USE_OMP       /* OpenMP = a must! */
# include <omp.h>
#endif

/** included here to be compiled with OpenMP, unlike the versions in libmri **/

#include "mri_genalign.c"
#include "mri_genalign_util.c"

/** include the warping functions, and enable the warp-optimizing functions **/

#define ALLOW_QWARP                /* this must be defined, or nothing works! */
#define ALLOW_QMODE                /* allows -Qfinal and -Qonly options */
#define ALLOW_PLUSMINUS            /* allows -plusminus option */
#undef  USE_PLUSMINUS_INITIALWARP  /* don't do this! */

#include "mri_nwarp.c"

/** include the Powell NEWUOA functions **/

#include "powell_int.c"
#define max MAX
#define min MIN
#include "powell_newuoa.c"
#undef max
#undef min

/** constants for the mri_weightize() function (liberated from 3dAllineate) **/

static int auto_weight    = 1 ;      /* 1=weighted 2=binary 3=binary+box */
static float auto_wclip   = 0.0f ;   /* user-defined clip level */
static float auto_wpow    = 1.0f ;   /* raise weight to this power */
static int auto_dilation  = 5 ;      /* dilation of binarized mask */
static float wt_medsmooth = 2.25f ;  /* median radius for weight smooth */
static float wt_gausmooth = 4.50f ;  /* Gaussian radius for weight smooth */

/*---------------------------------------------------------------------------*/
/*! Turn an input image into a weighting factor.
      If acod == 2, then make a binary mask at the end.
      If acod == 3, then make a boxed binary mask at the end.
    This function is taken from 3dAllineate.c.
-----------------------------------------------------------------------------*/

MRI_IMAGE * mri_weightize( MRI_IMAGE *im, int acod, int ndil, float aclip, float apow )
{
   float *wf,clip,clip2 ;
   int xfade,yfade,zfade , nx,ny,nz,nxy,nxyz , ii,jj,kk,ff ;
   byte *mmm ;
   MRI_IMAGE *qim , *wim ;

   if( Hverb ) INFO_message("Weightizing the base image") ;

   /*-- copy input image so we can mangle it --*/

   qim = mri_to_float(im) ; wf = MRI_FLOAT_PTR(qim) ;
   nx = qim->nx; ny = qim->ny; nz = qim->nz; nxy = nx*ny; nxyz = nxy*nz;
   for( ii=0 ; ii < nxyz ; ii++ ) wf[ii] = fabsf(wf[ii]) ;

   /*-- zero out along the edges so the edges of the volume get no weight --*/

#undef  WW
#define WW(i,j,k) wf[(i)+(j)*nx+(k)*nxy]

   xfade = (int)(0.05*qim->nx+3.0) ;                 /* number of points */
   yfade = (int)(0.05*qim->ny+3.0) ;                 /* along each face */
   zfade = (int)(0.05*qim->nz+3.0) ;                 /* to set to zero */
   if( 5*xfade >= qim->nx ) xfade = (qim->nx-1)/5 ;
   if( 5*yfade >= qim->ny ) yfade = (qim->ny-1)/5 ;
   if( 5*zfade >= qim->nz ) zfade = (qim->nz-1)/5 ;
   if( Hverb > 1 )
     ININFO_message("  xfade=%d yfade=%d zfade=%d",xfade,yfade,zfade);
   for( jj=0 ; jj < ny ; jj++ )
    for( ii=0 ; ii < nx ; ii++ )
     for( ff=0 ; ff < zfade ; ff++ ) WW(ii,jj,ff) = WW(ii,jj,nz-1-ff) = 0.0f;
   for( kk=0 ; kk < nz ; kk++ )
    for( jj=0 ; jj < ny ; jj++ )
     for( ff=0 ; ff < xfade ; ff++ ) WW(ff,jj,kk) = WW(nx-1-ff,jj,kk) = 0.0f;
   for( kk=0 ; kk < nz ; kk++ )
    for( ii=0 ; ii < nx ; ii++ )
     for( ff=0 ; ff < yfade ; ff++ ) WW(ii,ff,kk) = WW(ii,ny-1-ff,kk) = 0.0f;

   if( aclip > 0.0f ){  /* this is zero in 3dQwarp [31 Jul 2007] */
     int nleft , nclip ;
     for( nclip=nleft=ii=0 ; ii < nxyz ; ii++ ){
       if( wf[ii] > 0.0f ){
         if( wf[ii] < aclip ){ nclip++; wf[ii] = 0.0f; } else nleft++ ;
       }
     }
     if( Hverb > 1 )
       ININFO_message("  user clip=%g #clipped=%d #left=%d",
                      aclip,nclip,nleft) ;
   }

   /*-- squash super-large values down to reasonability --*/

   clip = 3.0f * THD_cliplevel(qim,0.5f) ;  /* defines 'super-large' */
   if( Hverb > 1 ) ININFO_message("  (unblurred) top clip=%g",clip) ;
   for( ii=0 ; ii < nxyz ; ii++ ) if( wf[ii] > clip ) wf[ii] = clip ;

   /*-- blur a little: median then Gaussian;
          the idea is that the median filter smashes localized spikes,
          then the Gaussian filter does a litte extra general smoothing. --*/

   mmm = (byte *)malloc( sizeof(byte)*nxyz ) ;         /* make a mask */
   for( ii=0 ; ii < nxyz ; ii++ ) mmm[ii] = (wf[ii] > 0.0f) ;
   if( wt_medsmooth > 0.0f ){
     wim = mri_medianfilter( qim , wt_medsmooth , mmm , 0 ) ;
     mri_free(qim) ;  /* no longer needed */
   } else {
     wim = qim ;      /* no median filter */
   }
   wf = MRI_FLOAT_PTR(wim) ;
   if( wt_gausmooth > 0.0f )                     /* Gaussian filter */
     FIR_blur_volume_3d( wim->nx , wim->ny , wim->nz ,
                         1.0f , 1.0f , 1.0f ,  wf ,
                         wt_gausmooth , wt_gausmooth , wt_gausmooth ) ;

   /*-- clip off small values, and
        keep only the largest cluster of supra threshold voxels --*/

   clip  = 0.05f * mri_max(wim) ;             /* define 'msall' */
   clip2 = 0.33f * THD_cliplevel(wim,0.33f) ; /* values here */
   clip  = MAX(clip,clip2) ;
   if( Hverb > 1 ) ININFO_message("  (blurred) bot clip=%g",clip) ;
   for( ii=0 ; ii < nxyz ; ii++ ) mmm[ii] = (wf[ii] >= clip) ;
   /* get biggest cluster, erode it, re-cluster that */
   THD_mask_clust( nx,ny,nz, mmm ) ;
   THD_mask_erode( nx,ny,nz, mmm, 1 ) ;  /* cf. thd_automask.c */
   THD_mask_clust( nx,ny,nz, mmm ) ;
   /* kill anyone not in the surviving cluster */
   for( ii=0 ; ii < nxyz ; ii++ ) if( !mmm[ii] ) wf[ii] = 0.0f ;
   free((void *)mmm) ;  /* free the mask */

   /*-- convert weight to 0..1 range [10 Sep 2007] --*/

   clip = 0.0f ;
   for( ii=0 ; ii < nxyz ; ii++ ) if( wf[ii] > clip ) clip = wf[ii] ;
   if( clip == 0.0f )
     ERROR_exit("Can't compute autoweight: max value seen as 0") ;
   clip = 1.0f / clip ;
   for( ii=0 ; ii < nxyz ; ii++ ) wf[ii] *= clip ;

   /*-- take a power? --*/

   if( apow > 0.0f && apow != 1.0f ){
     if( Hverb > 1 ) ININFO_message("  raising to %g power",apow) ;
     for( ii=0 ; ii < nxyz ; ii++ )
       if( wf[ii] > 0.0f ) wf[ii] = powf( wf[ii] , apow ) ;
   }

   /*-- binarize (acod==2)?  boxize (acod==3)? --*/

#undef  BPAD
#define BPAD 4
   if( acod == 2 || acod == 3 ){  /* binary weight: mask=2 or maskbox=3 */
     if( Hverb > 1 ) ININFO_message("  binarizing") ;
     for( ii=0 ; ii < nxyz ; ii++ ) if( wf[ii] != 0.0f ) wf[ii] = 1.0f ;
     if( ndil > 0 ){  /* 01 Mar 2007: dilation */
       byte *mmm = (byte *)malloc(sizeof(byte)*nxyz) ;
       if( Hverb > 1 ) ININFO_message("  dilating") ;
       for( ii=0 ; ii < nxyz ; ii++ ) mmm[ii] = (wf[ii] != 0.0f) ;
       for( ii=0 ; ii < ndil ; ii++ ){
         THD_mask_dilate     ( nx,ny,nz , mmm , 3 ) ;
         THD_mask_fillin_once( nx,ny,nz , mmm , 2 ) ;
       }
       for( ii=0 ; ii < nxyz ; ii++ ) wf[ii] = (float)mmm[ii] ;
       free(mmm) ;
     }
     if( acod == 3 ){  /* boxize */
       int xm,xp , ym,yp , zm,zp ;
       MRI_autobbox_clust(0) ;
       MRI_autobbox( wim , &xm,&xp , &ym,&yp , &zm,&zp ) ;
       xm -= BPAD ; if( xm < 1    ) xm = 1 ;
       ym -= BPAD ; if( ym < 1    ) ym = 1 ;
       zm -= BPAD ; if( zm < 1    ) zm = 1 ;
       xp += BPAD ; if( xp > nx-2 ) xp = nx-2 ;
       yp += BPAD ; if( yp > ny-2 ) yp = ny-2 ;
       zp += BPAD ; if( zp > nz-2 ) zp = nz-2 ;
       if( Hverb > 1 )
         ININFO_message("  box=%d..%d X %d..%d X %d..%d = %d voxels",
                        xm,xp , ym,yp , zm,zp , (xp-xm+1)*(yp-ym+1)*(zp-zm+1) ) ;
       for( kk=zm ; kk <= zp ; kk++ )
        for( jj=ym ; jj <= yp ; jj++ )
         for( ii=xm ; ii <= xp ; ii++ ) WW(ii,jj,kk) = 1.0f ;
     }
   }

   return wim ;
}

/*---------------------------------------------------------------------------*/

static THD_3dim_dataset *qset = NULL ;

#undef USE_SAVER  /* was for ITEROUT (mri_nwarp.c) but doesn't work any more */
#ifdef USE_SAVER
void Qsaver(char *lab, MRI_IMAGE *im)
{
   static int first=1 ;

   if( im == NULL || qset == NULL ) return ;

   if( first ){
     EDIT_substitute_brick( qset, 0, MRI_float,  MRI_FLOAT_PTR(im) ) ; first = 0 ;
   } else {
     EDIT_add_brick( qset, MRI_float, 0.0f, MRI_FLOAT_PTR(im) ) ;
   }
   if( lab != NULL && *lab != '\0' )
     EDIT_BRICK_LABEL(qset,DSET_NVALS(qset)-1,lab) ;

   mri_clear_data_pointer(im) ; return ;
}
#endif

/*---------------------------------------------------------------------------*/
/* Help me if you can, I'm feeling down down down */
/*---------------------------------------------------------------------------*/

void Qhelp(void)
{
  printf("\n") ;
  printf("Usage: 3dQwarp [OPTIONS]\n") ;
  printf(
    "\n"
    "* Computes a nonlinearly warped version of source_dataset to match base_dataset.\n"
    " ++ The detail allowed in the warping is set by the '-minpatch' option.\n"
    " ++ The discrete warp computed herein is a representation of an underlying\n"
    "    piecewise polynomial C1 diffeomorphism.\n"
    "\n"
    "* Input datasets must be on the same 3D grid!\n"
    " ++ However, You can use the '-allineate' option in 3dQwarp to do\n"
    "    affine alignment before the nonlinear alignment, which will also\n"
    "    resample the aligned source image to the base dataset grid.\n"
    " ++ OR, you can use the '-resample' option in 3dQwarp to resample the\n"
    "    source dataset to the base grid before doing the nonlinear stuff,\n"
    "    without doing any preliminary affine alignment.\n"
    "\n"
    "* Input datasets should be reasonably well aligned already\n"
    "  (e.g., as from an affine warping via 3dAllineate).\n"
    " ++ The standard result from 3dAllineate will resample the affinely\n"
    "    aligned dataset to the same 3D grid as the -base dataset, so this\n"
    "    new dataset will be ready to run in 3dQwarp against the same base.\n"
    " ++ Again, the '-allineate' option can now do this for you, inside 3dQwarp.\n"
    "\n"
    "* Outputs of 3dQwarp are the warped dataset and the warp that did it.\n"
    " ++ These datasets are stored in float format, no matter what the\n"
    "    data type of the source dataset.\n"
    "\n"
    "* Matching by default is the 'clipped Pearson' method, and\n"
    "  can be changed to 'pure Pearson' with the '-pear' option.\n"
    " ++ The purpose of 'clipping' is to reduce the impact of outlier values\n"
    "    (small or large) on the correlation.\n"
    " ++ For the adventurous, you can also try these matching functions:\n"
    "      '-hel' for Hellinger distance\n"
    "      '-mi'  for Mutual Information\n"
    "      '-nmi' for Normalized Mutual Information\n"
    "    These options have NOT been extensively tested for usefulness,\n"
    "    and should be considered experimental at this infundibulum.\n"
    " ++ The 'local' correlation options are also now available:\n"
    "      '-lpc' for Local Pearson minimization (i.e., EPI-T1 registration)\n"
    "      '-lpa' for Local Pearson maximization\n"
    " ** If you use '-lpc', then '-maxlev 0' is automatically set.  If you want\n"
    "    to go to more refined levels, you can set '-maxlev' AFTER '-lpc' on the\n"
    "    command line.\n"
    " ** For aligning EPI to T1, the '-lpc' option can be used; my advice\n"
    "    would be to do something like the following:\n"
    "      3dSkullStrip -input SUBJ_anat+orig -prefix SUBJ_anatSS\n"
    "      3dbucket -prefix SUBJ_epiz SUBJ_epi+orig'[0]'\n"
    "      align_epi_anat.py -anat SUBJ_anat+orig                            \\\n"
    "                        -epi SUBJ_epiz+orig -epi_base 0 -partial_axial  \\\n"
    "                        -epi2anat -master_epi SUBJ_anat+orig            \\\n"
    "                        -big_move\n"
    "      3dQwarp -source SUBJ_anatSS+orig.HEAD   \\\n"
    "                -base   SUBJ_epiz_al+orig     \\\n"
    "                -prefix SUBJ_anatSSQ          \\\n"
    "                -lpc -verb -iwarp -blur 0 3\n"
    "      3dNwarpApply -nwarp  SUBJ_anatSSQ_WARPINV+orig  \\\n"
    "                   -source SUBJ_epiz_al+orig          \\\n"
    "                   -prefix SUBJ_epiz_alQ\n"
    "    * Zeroth, the T1 is prepared by skull stripping and the EPI is prepared\n"
    "      by extracting just the 0th sub-brick for registration purposes.\n"
    "    * First, the EPI is aligned to the T1 using the affine 3dAllineate, and\n"
    "      at the same time resampled to the T1 grid (via align_epi_anat.py).\n"
    "    * Second, it is nonlinearly aligned ONLY using the global warping -- it is\n"
    "      futile to try to align such dissimilar image types more precisely.\n"
    "    * The EPI is used as the base in 3dQwarp so that it provides the weighting,\n"
    "      and so partial brain coverage (as long as it covers MOST of the brain)\n"
    "      should not cause a problem (we hope).\n"
    "    * Third, 3dNwarpApply is used to take the inverse warp from 3dQwarp to\n"
    "      transform the EPI to the T1 space, since 3dQwarp transformed the T1 to\n"
    "      EPI space.\n"
    "    * Someday, this procedure may be incorporated into align_epi_anat.py :-)\n"
    "  ** It is vitally important to visually look at the results of this process! **\n"
#if defined(USE_OMP) && defined(__GNU_C__)
    " ++ Note that these 'adventurous' matching options may cause trouble with\n"
    "    OpenMP compiled with GNU gcc, due to a bug in gcc's OpenMP library\n"
    "    -- and this binary is compiled that way!\n"
    "    -- this issue is one reason these options are labeled 'adventurous'.\n"
#endif
    "\n"
    "* For aligning T1-weighted anatomical volumes, Zhark recommends that\n"
    "  you use the 3dUnifize program to (approximately) spatially uniformize\n"
    "  and normalize their intensities -- this helps in the matching process,\n"
    "  especially when using datasets from different scanners.\n"
    " ++ Skull stripping a la 3dSkullStrip is also a good idea (prior to 3dUnifize),\n"
    "    even if you are registering datasets from the same subject; see the\n"
    "    SAMPLE USAGE section below for an example.\n"
    " ++ If you ultimately want a non-3dUnifize-d transformed dataset, you can use\n"
    "    the output WARP dataset and 3dNwarpApply to transform the un-3dUnifize-d\n"
    "    source dataset; again, see the SAMPLE USAGE section below.\n"
    "\n"
    "* If for some deranged reason you have datasets with very non-cubical voxels,\n"
    "  they should be resampled to a cubical grid before trying 3dQwarp.  For example,\n"
    "  if you have acquired 1x1x4 mm T1-weighted structural volumes (why?), then\n"
    "  resample them to 1x1x1 mm before doing any other registration processing.\n"
    "  For example:\n"
    "    3dAllineate -input anatT1_crude+orig -newgrid 1.0 \\\n"
    "                -prefix anatT1_fine -final wsinc5     \\\n"
    "                -1Dparam_apply '1D: 12@0'\\'\n"
    "  This operation can also now be done using the '-allineate' or '-resample'\n"
    "  options to 3dQwarp.\n"
    "\n"
    "** Please note that this program is very CPU intensive, and is what computer\n"
    "   scientists call a 'pig' (i.e., run time from 10s of minutes to hours).\n"
#ifndef USE_OMP
    " ++ It would be best to run 3dQwarp on a multi-CPU computer, using a binary\n"
    "    compiled with the OpenMP library. Unfortunately, this particular version is\n"
    "    NOT built with OpenMP, and you will probably find it to be unbearably slow :-(\n"
#endif
    "\n"
    "------------\n"
    "SAMPLE USAGE\n"
    "------------\n"
    "* For registering a T1-weighted anat to a mildly blurry template at about\n"
    "  a 1x1x1 mm resolution (note that the 3dAllineate step, to give the\n"
    "  preliminary alignment, will also produce a dataset on the same 3D grid\n"
    "  as the TEMPLATE+tlrc dataset, which 3dQwarp requires):\n"
    "\n"
    "    3dUnifize -prefix anatT1_U -input anatT1+orig\n"
    "    3dSkullStrip -input anatT1_U+orig -prefix anatT1_US -niter 400 -ld 40\n"
    "    3dAllineate -prefix anatT1_USA -base TEMPLATE+tlrc    \\\n"
    "                -source anatT1_US+orig -twopass -cost lpa \\\n"
    "                -1Dmatrix_save anatT1_USA.aff12.1D        \\\n"
    "                -autoweight -fineblur 3 -cmass\n"
    "    3dQwarp -prefix anatT1_USAQ -duplo -blur 0 3 \\\n"
    "            -base TEMPLATE+tlrc -source anatT1_USA+tlrc\n"
    "\n"
    "  You can then use the anatT1_USAQ_WARP+tlrc dataset to transform other\n"
    "  datasets (that were aligned with the input anatT1+orig) in the same way\n"
    "  using program 3dNwarpApply, as in\n"
    "\n"
    "    3dNwarpApply -nwarp 'anatT1_USAQ_WARPtlrc anatT1_USA.aff12.1D' \\\n"
    "                 -source NEWSOURCE+orig -prefix NEWSOURCE_warped\n"
    "\n"
    "  For example, if you want a warped copy of the original anatT1+orig dataset\n"
    "  (without the 3dUnifize and 3dSkullStrip modifications), put 'anatT1' in\n"
    "  place of 'NEWSOURCE' in the above command.\n"
    "\n"
    "  Note that the '-nwarp' option to 3dNwarpApply has TWO filenames inside\n"
    "  single quotes.  This feature tells that program to compose (catenate) those\n"
    "  2 spatial transformations before applying the resulting warp.  See the -help\n"
    "  output of 3dNwarpApply for more sneaky/cunning ways to make the program warp\n"
    "  datasets (and also see the example just below).\n"
    "\n"
    "   ** PLEASE NOTE that if you use the '-allineate' option in 3dQwarp, to   **\n"
    "   ** do the 3dAllineate step inside 3dQwarp, then you do NOT catenate     **\n"
    "   ** the affine and nonlinear warps as in the 3dNwarpApply example above, **\n"
    "   ** since the output nonlinear warp will ALREADY have be catenated with  **\n"
    "   ** the affine warp -- this output warp is the transformation directly   **\n"
    "   ** between the '-source' and '-base' datasets (as is reasonable IZHO).  **\n"
    "\n"
    "  If the NEWSOURCE+orig dataset is integer-valued (e.g., anatomical labels),\n"
    "  then you would use the '-ainterp NN' with 3dNwarpApply, to keep the program\n"
    "  from interpolating the voxel values.\n"
    "\n"
    "* If you use align_epi_anat.py to affinely transform several EPI datasets to\n"
    "  match a T1 anat, and then want to nonlinearly warp the EPIs to the template,\n"
    "  following the warp generated above, the procedure is something like this:\n"
    "\n"
    "    align_epi_anat.py -anat anatT1+orig -epi epi_r1+orig \\\n"
    "                      -epi_base 3 -epi2anat -big_move    \\\n"
    "                      -child_epi epi_r2+orig epi_r3+orig\n"
    "\n"
    "    3dNwarpApply -source epi_r1+orig                                \\\n"
    "                 -nwarp 'anatT1_USAQ_WARP+tlrc anatT1_USA.aff12.1D' \\\n"
    "                 -affter epi_r1_al_reg_mat.aff12.1D                 \\\n"
    "                 -master WARP -newgrid 2.0                          \\\n"
    "                 -prefix epi_r1_AQ\n"
    "\n"
    "    (mutatis mutandis for 'child' datasets epi_r2, epi_r3, etc.).\n"
    "\n"
    "  The above procedure transforms the data directly from the un-registered\n"
    "  original epi_r1+orig dataset, catenating the EPI volume registration\n"
    "  transformations (epi_r1_al_reg_mat.aff12.1D) with the affine anat to\n"
    "  template transformation (anatT1_USA.aff12.1D) and with the nonlinear\n"
    "  anat to template transformation (anatT1_USAQ_WARP+tlrc).  3dNwarpApply\n"
    "  will use the default 'wsinc5' interpolation method, which does not blur\n"
    "  the results much -- an important issue for statistical analysis of the\n"
    "  EPI time series.\n"
    "\n"
    "  Various functions, such as volume change fraction (Jacobian determinant)\n"
    "  can be calculated from the warp dataset via program 3dNwarpFuncs.\n"
    "\n"
    "--------------------\n"
    "COMMAND LINE OPTIONS\n"
    "--------------------\n"
    " -base   base_dataset   = Alternative way to specify the base dataset.\n"
    " -source source_dataset = Alternative way to specify the source dataset.\n"
    "                         * You can either use both '-base' and '-source',\n"
    "                           OR you can put the base and source dataset\n"
    "                           names last on the command line.\n"
    "                         * But you cannot use just one of '-base' or '-source'\n"
    "                           and then put the other input dataset name at the\n"
    "                           end of the command line!\n"
    "\n"
    " -prefix ppp  = Sets the prefix for the output datasets.\n"
    "               * The source dataset is warped to match the base\n"
    "                 and gets prefix 'ppp'. (Except if '-plusminus' is used.)\n"
    "               * The final interpolation to this output dataset is\n"
    "                 done using the 'wsinc5' method.  See the output of\n"
    "                   3dAllineate -HELP\n"
    "                 (in the \"Modifying '-final wsinc5'\" section) for\n"
    "                 the technical details.\n"
    "               * The 3D warp used is saved in a dataset with\n"
    "                 prefix 'ppp_WARP' -- this dataset can be used\n"
    "                 with 3dNwarpApply and 3dNwarpCat, for example.\n"
    "                 * To be clear, this is the warp from source dataset\n"
    "                   coordinates to base dataset coordinates, where the\n"
    "                   values at each base grid point are the xyz displacments\n"
    "                   needed to move that grid point's xyz values to the\n"
    "                   corresponding xyz values in the source dataset:\n"
    "                     base( (x,y,z) + WARP(x,y,z) ) matches source(x,y,z)\n"
    "                   Another way to think of this warp is that it 'pulls'\n"
    "                   values back from source space to base space.\n"
    "               * 3dNwarpApply would use 'ppp_WARP' to transform datasets\n"
    "                 aligned with the source dataset to be aligned with the\n"
    "                 base dataset.\n"
    "              ** If you do NOT want this warp saved, use the option '-nowarp'.\n"
    "            -->> (However, this warp is usually the most valuable possible output!)\n"
    "               * If you want to calculate and save the inverse 3D warp,\n"
    "                 use the option '-iwarp'.  This inverse warp will then be\n"
    "                 saved in a dataset with prefix 'ppp_WARPINV'.\n"
    "               * This inverse warp could be used to transform data from base\n"
    "                 space to source space, if you need to do such an operation.\n"
    "               * You can easily compute the inverse later, say by a command like\n"
    "                   3dNwarpCat -prefix Z_WARPINV 'INV(Z_WARP+tlrc)'\n"
    "\n"
    " -allineate   = This option will make 3dQwarp run 3dAllineate first, to align\n"
    "   *OR*         the source dataset to the base with an affine transformation.\n"
    " -allin         It will then use that alignment as a starting point for the\n"
    "   *OR*         nonlinear warping.\n"
    " -allinfast    * With -allineate, the source dataset does NOT have to be on\n"
    "                 the same 3D grid as the base, since the intermediate output\n"
    "                 of 3dAllineate (the substitute source) will be on the grid\n"
    "                 as the base.\n"
    "               * If the datasets overlap reasonably already, you can use the\n"
    "                 option '-allinfast' (instead of '-allineate') to add the\n"
    "                 options '-onepass -norefinal' to the 3dAllineate command\n"
    "                 line, to make it run faster.\n"
    "          -->>** The final output warp dataset is the warp directly between\n"
    "                 the original source dataset and the base (i.e., the catenation\n"
    "                 of the affine matrix from 3dAllineate and the nonlinear warp\n"
    "                 from the 'warpomatic' procedure in 3dQwarp).\n"
    "              ** The above point means that you should NOT NOT NOT use the affine\n"
    "                 warp output by the '-allineate' option in combination with the\n"
    "                 nonlinear warp output by 3dQwarp (say, when using 3dNwarpApply),\n"
    "                 since the affine warp would then be applied twice -- which would\n"
    "                 be WRONG WRONG WRONG.\n"
    "          -->>** The final output warped dataset is warped directly from the\n"
    "                 original source dataset, NOT from the substitute source.\n"
    "               * The intermediate files from 3dAllineate (the substitute source\n"
    "                 dataset and the affine matrix) are saved, using 'prefix_Allin'\n"
    "                 in the filenames.  If you wish to have them deleted, use the\n"
    "                 option '-allinkill' in addition to '-allineate'.\n"
    "             *** The following 3dQwarp options CANNOT be used with -allineate:\n"
    "                   -plusminus  -inilev  -iniwarp\n"
    "             *** However, you CAN use -duplo with -allineate.\n"
    "\n"
    " -allineate_opts '-opt ...'\n"
    "   *OR*        * This option lets you add extra options to the 3dAllineate\n"
    " -allopt         command to be run by 3dQwarp.  Normally, you won't need\n"
    "                 to do this.\n"
    "               * All the extra options for the 3dAllineate command line\n"
    "                 should be enclosed inside a pair of quote marks; e.g.,\n"
    "                    -allopt '-cost lpa -verb'\n"
    "               * If '-emask' is used in 3dQwarp, the same option will be\n"
    "                 passed to 3dAllineate automatically, so you don't have to\n"
    "                 do that yourself.\n"
    "             *** Do NOT attempt to use the (obsolescent) '-nwarp' option in\n"
    "                 3dAllineate from inside 3dQwarp -- bad things will probably\n"
    "                 happen, and you won't EVER get any Christmas presents again!\n"
    "\n"
    " -resample    = This option simply resamples the source dataset to match the\n"
    "                base dataset grid.  You can use this if the two datasets\n"
    "                overlap well (as seen in the AFNI GUI), but are not on the\n"
    "                same 3D grid.\n"
    "               * If they don't overlap well, use -allineate instead.\n"
    "               * As with -allineate, the final output dataset is warped\n"
    "                 directly from the source dataset, not from the resampled\n"
    "                 source dataset.\n"
    "               * The reampling here (and with -allineate) is done with the\n"
    "                 'wsinc5' method, which has very little blurring artifact.\n"
    "               * If the base and source datasets ARE on the same 3D grid,\n"
    "                 then the -resample option will be ignored.\n"
    "               * You CAN use -resample with these 3dQwarp options:\n"
    "                   -plusminus  -inilev  -iniwarp  -duplo\n"
    "\n"
    " -nowarp      = Do not save the _WARP file.\n"
    " -iwarp       = Do compute and save the _WARPINV file.\n"
    " -nodset      = Do not save the warped source dataset (i.e., if you only need the _WARP).\n"
    "\n"
    " -pear        = Use strict Pearson correlation for matching.\n"
    "               * Not usually recommended, since the 'clipped Pearson' method\n"
    "                 used by default will reduce the impact of outlier values.\n"
#if 0
    " -localstat   = Normally, the correlation being optimized is computed globally\n"
    "                (over the entire volume).  With this option, the correlation is\n"
    "                computed over only the patch being worked on at any given moment.\n"
    "               * If you use '-verb', you'll see that the cost functional (negative\n"
    "                 of the correlation) decreases as the program progresses.  With\n"
    "                 '-localstat', this continual decrease will not happen, and the\n"
    "                 reported cost will go up and down as the patches vary.  However,\n"
    "                 the program will still give a good match (one hopes and prays).\n"
#endif
    "\n"
    " -noneg       = Replace negative values in either input volume with 0.\n"
    "               * If there ARE negative input values, and you do NOT use -noneg,\n"
    "                 then strict Pearson correlation will be used, since the 'clipped'\n"
    "                 method only is implemented for non-negative volumes.\n"
    "\n"
    " -nopenalty   = Don't use a penalty on the cost function; the goal\n"
    "                of the penalty is to reduce grid distortions.\n"
    "               * If there penalty is turned off AND you warp down to\n"
    "                 a fine scale (e.g., '-minpatch 11'), you will probably\n"
    "                 get strange-looking results.\n"
    " -penfac ff   = Use the number 'ff' to weight the penalty.\n"
    "                The default value is 1.  Larger values of 'ff' mean the\n"
    "                penalty counts more, reducing grid distortions,\n"
    "                insha'Allah; '-nopenalty' is the same as '-penfac 0'.\n"
    "           -->>* [23 Sep 2013] -- Zhark increased the default value of\n"
    "                 the penalty by a factor of 5, and also made it get\n"
    "                 progressively larger with each level of refinement.\n"
    "                 Thus, warping results will vary from earlier instances\n"
    "                 of 3dQwarp.\n"
    "               * The progressive increase in the penalty at higher levels\n"
    "                 means that the 'cost function' can actually look like the\n"
    "                 alignment is getting worse when the levels change.\n"
    "               * IF you wish to turn off this progression, for whatever\n"
    "                 reason (e.g., to keep compatibility with older results),\n"
    "                 use the option '-penold'.  To be completely compatible with\n"
    "                 the older 3dQwarp, you'll also have to use '-penfac 0.2'.\n"
    "\n"
    " -useweight   = With '-useweight', each voxel in the base automask is weighted\n"
    "                by the intensity of the (blurred) base image.  This makes\n"
    "                white matter count more in T1-weighted volumes, for example.\n"
    "           -->>* [24 Mar 2014] This option is is now the default.\n"
    " -noweight    = If you want a binary weight (the old default), use this option.\n"
    "                That is, each voxel in the base volume automask will be\n"
    "                weighted the same in the computation of the cost functional.\n"
    " -weight www  = Instead of computing the weight from the base dataset,\n"
    "                directly input the weight volume from dataset 'www'.\n"
    "               * Useful if you know what over parts of the base image you\n"
    "                 want to emphasize or de-emphasize the matching functional.\n"
    "\n"
    " -blur bb     = Gaussian blur the input images by 'bb' (FWHM) voxels before\n"
    "                doing the alignment (the output dataset will not be blurred).\n"
    "                The default is 2.345 (for no good reason).\n"
    "               * Optionally, you can provide 2 values for 'bb', and then\n"
    "                 the first one is applied to the base volume, the second\n"
    "                 to the source volume.\n"
    "           -->>* e.g., '-blur 0 3' to skip blurring the base image\n"
    "                 (if the base is a blurry template, for example).\n"
    "               * A negative blur radius means to use 3D median filtering,\n"
    "                 rather than Gaussian blurring.  This type of filtering will\n"
    "                 better preserve edges, which can be important in alignment.\n"
    "               * If the base is a template volume that is already blurry,\n"
    "                 you probably don't want to blur it again, but blurring\n"
    "                 the source volume a little is probably a good idea, to\n"
    "                 help the program avoid trying to match tiny features.\n"
    "               * Note that -duplo will blur the volumes some extra\n"
    "                 amount for the initial small-scale warping, to make\n"
    "                 that phase of the program converge more rapidly.\n"
    "\n"
    " -emask ee    = Here, 'ee' is a dataset to specify a mask of voxels\n"
    "                to EXCLUDE from the analysis -- all voxels in 'ee'\n"
    "                that are NONZERO will not be used in the alignment.\n"
    "               * The base image always automasked -- the emask is\n"
    "                 extra, to indicate voxels you definitely DON'T want\n"
    "                 included in the matching process, even if they are\n"
    "                 inside the brain.\n"
    "           -->>* Note that 3dAllineate has the same option. Since you\n"
    "                 usually have to use 3dAllineate before 3dQwarp, you\n"
    "                 will probably want to use -emask in both programs.\n"
    "                 [ Unless, of course, you are using '-allineate',  which  ]\n"
    "                 [ will automatically include '-emask' in the 3dAllineate ]\n"
    "                 [ phase if '-emask' is used here in 3dQwarp.             ]\n"
    "               * Applications: exclude a tumor or resected region\n"
    "                 (e.g., draw a mask in the AFNI Drawing plugin).\n"
    "           -->>* Note that the emask applies to the base dataset,\n"
    "                 so if you are registering a pre- and post-surgery\n"
    "                 volume, you would probably use the post-surgery\n"
    "                 dataset as the base.  If you eventually want the\n"
    "                 result back in the pre-surgery space, then you\n"
    "                 would use the inverse warp afterwards.\n"
    "\n"
    " -noXdis      = These options let you specify that the warp should not\n"
    " -noYdis      = displace in the given direction.  For example, combining\n"
    " -noZdis      = -noXdis and -noZdis would mean only warping along the\n"
    "                y-direction would be allowed.\n"
    "               * Here, 'x' refers to the first coordinate in the dataset,\n"
    "                 which is usually the Right-to-Left direction.  Et cetera.\n"
#if 0
    "               * xyz coordinates herein refer to the DICOM order, where\n"
    "                   -x = Right  -y = Anterior   -z = Inferior\n"
    "                   +x = Left   +y = Posterior  +z = Superior\n"
#endif
    "\n"
    " -iniwarp ww  = 'ww' is a dataset with an initial nonlinear warp to use.\n"
    "               * If this option is not used, the initial warp is the identity.\n"
    "               * You can specify a catenation of warps (in quotes) here, as in\n"
    "                 program 3dNwarpApply.\n"
    "               * As a special case, if you just input an affine matrix in a .1D\n"
    "                 file, that will work also -- it is treated as giving the initial\n"
    "                 warp via the string \"IDENT(base_dataset) matrix_file.aff12.1D\".\n"
    "               * You CANNOT use this option with -duplo !!\n"
    "               * -iniwarp is usually used with -inilev to re-start 3dQwarp from\n"
    "                 a previous stopping point.\n"
#if 0
    "               * Special cases allow the creation of an initial affine 'warp'\n"
    "                 from a list of 12 numbers:\n"
    "                 * 'MATRIX(a11,a12,a13,a14,a21,a22,a23,a24,a31,a32,a33,a44)'\n"
    "                   provides the coordinate transformation matrix directly\n"
    "                   as might come from 3dAllineate's '-1Dmatrix_save' option.\n"
    "                 * 'PARAM(dx,dy,dz,za,xa,ya,sx,sy,sz,xs,ys,zs)'\n"
    "                   provides the 3 shift, 3 angle, 3 scale, and 3 shear\n"
    "                   parameters, as would come from 3dAllineate's\n"
    "                   '-1Dparam_save' option.\n"
    "                 * The numeric parameters can be separated by commas\n"
    "                   or blanks.  The closing ')' isn't really required, but\n"
    "                   the opening '(' after 'MATRIX' or 'PARAM' is needed.\n"
    "                   For this reason, you will probably need to put this\n"
    "                   argument inside 'single' or \"double\" quotes, to protect\n"
    "                   it from interpretation by the Unix shell.\n"
#endif
    "\n"
    " -inilev lv   = 'lv' is the initial refinement 'level' at which to start.\n"
    "               * Usually used with -iniwarp; CANNOT be used with -duplo.\n"
    "               * The combination of -inilev and -iniwarp lets you take the\n"
    "                 results of a previous 3dQwarp run and refine them further:\n"
    "                   3dQwarp -prefix Q25 -source SS+tlrc -base TEMPLATE+tlrc \\\n"
    "                           -duplo -minpatch 25 -blur 0 3\n"
    "                   3dQwarp -prefix Q11 -source SS+tlrc -base TEMPLATE+tlrc \\\n"
    "                           -inilev 7 -iniwarp Q25_WARP+tlrc -blur 0 2\n"
    "                 Note that the source dataset in the second run is the SAME as\n"
    "                 in the first run.  If you don't see why this is necessary,\n"
    "                 then you probably need to seek help from an AFNI guru.\n"
    "          -->>** Also see the script @toMNI_Qwarpar for the use of this option\n"
    "                 in creating a template dataset from a collection of scans from\n"
    "                 different subjects.\n"
    "\n"
    " -minpatch mm = Set the minimum patch size for warp searching to 'mm' voxels.\n"
    "   *OR*        * The value of mm should be an odd integer.\n"
    " -patchmin mm  * The default value of mm is 25.\n"
    "               * For more accurate results than mm=25, try 19 or 13.\n"
    "               * The smallest allowed value is 9 (which will be VERY slow).\n"
#ifdef USE_SAVER
    "               * If you want to see the warped results at various levels\n"
    "                 of patch size, use the '-qsave' option.\n"
#endif
    "\n"
    " -maxlev lv   = Here, 'lv' is the maximum refinement 'level' to use.  This\n"
    "                is an alternate way to specify when the program should stop.\n"
    "               * To only do global polynomial warping, use '-maxlev 0'.\n"
    "               * If you use both '-minpatch' and '-maxlev', then you are\n"
    "                 living on the edge of danger.\n"
    "               * Of course, I know that you LIVE for such thrills.\n"
    "\n"
    " -duplo       = Start off with 1/2 scale versions of the volumes,\n"
    "                for getting a speedy coarse first alignment.\n"
    "               * Then scales back up to register the full volumes.\n"
    "                 The goal is greater speed, and it seems to help this\n"
    "                 positively piggish program to be more expeditious.\n"
    "               * However, accuracy is somewhat lower with '-duplo',\n"
    "                 for reasons that currenly elude Zhark; for this reason,\n"
    "                 the Emperor does not usually use '-duplo'.\n"
    "\n"
    " -workhard    = Iterate more times, which can help when the volumes are\n"
    "                hard to align at all, or when you hope to get a more precise\n"
    "                alignment.\n"
    "               * Slows the program down (possibly a lot), of course.\n"
    "               * When you combine '-workhard'  with '-duplo', only the\n"
    "                 full size volumes get the extra iterations.\n"
    "               * For finer control over which refinement levels work hard,\n"
    "                 you can use this option in the form (for example)\n"
    "                     -workhard:4:7\n"
    "                 which implies the extra iterations will be done at levels\n"
    "                 4, 5, 6, and 7, but not otherwise.\n"
    "               * You can also use '-superhard' to iterate even more, but\n"
    "                 this extra option will REALLY slow things down.\n"
    "           -->>* Under most circumstances, you should not need to use either\n"
    "                 -workhard or -superhard.\n"
    "           -->>* The fastest way to register to a template image is via the\n"
    "                 -duplo option, and without the -workhard or -superhard options.\n"
#if 0
#ifdef ALLOW_QMODE
    "\n"
    " -Qfinal      = At the finest patch size (the final level), use Hermite\n"
    "                quintic polynomials for the warp instead of cubic polynomials.\n"
    "               * In a 3D 'patch', there are 2x2x2x3=24 cubic polynomial basis\n"
    "                 function parameters over which to optimize (2 polynomials\n"
    "                 dependent on each of the x,y,z directions, and 3 different\n"
    "                 directions of displacement).\n"
    "               * There are 3x3x3x3=81 quintic polynomial parameters per patch.\n"
    "               * With -Qfinal, the final level will have more detail in\n"
    "                 the allowed warps, at the cost of yet more CPU time.\n"
    "               * This option is also not usually needed, and is experimental.\n"
    "\n"
    " -Qonly       = Use Hermite quintic polynomials at all levels.\n"
    "               * Very slow (about 4 times longer).  Also experimental.\n"
    "               * Will produce a (discrete representation of a) C2 warp.\n"
#endif
#endif
#ifdef USE_SAVER
    "\n"
    " -qsave       = Save intermediate warped results as well, in a dataset\n"
    "                with '_SAVE' appended to the '-prefix' value.\n"
    "               * This allows you to see the amount of improvement at\n"
    "                 each patch refinement level, and may help you decide\n"
    "                 the size for '-minpatch' for future work.\n"
    "               * Otherwise, this option is mostly for debugging.\n"
#endif
#ifdef ALLOW_PLUSMINUS
    "\n"
    " -plusminus   = Normally, the warp displacements dis(x) are defined to match\n"
    "                base(x) to source(x+dis(x)).  With this option, the match\n"
    "                is between base(x-dis(x)) and source(x+dis(x)) -- the two\n"
    "                images 'meet in the middle'.\n"
    "               * One goal is to mimic the warping done to MRI EPI data by\n"
    "                 field inhomogeneities, when registering between a 'blip up'\n"
    "                 and a 'blip down' down volume, which will have opposite\n"
    "                 distortions.\n"
    "               * Define Wp(x) = x+dis(x) and Wm(x) = x-dis(x).  Then since\n"
    "                 base(Wm(x)) matches source(Wp(x)), by substituting INV(Wm(x))\n"
    "                 wherever we see x, we have base(x) matches source(Wp(INV(Wm(x))));\n"
    "                 that is, the warp V(x) that one would get from the 'usual' way\n"
    "                 of running 3dQwarp is V(x) = Wp(INV(Wm(x))).\n"
    "               * Conversely, we can calculate Wp(x) in terms of V(x) as follows:\n"
    "                   If V(x) = x + dv(x), define Vh(x) = x + dv(x)/2;\n"
    "                   then Wp(x) = V(INV(Vh(x)))\n"
    "               * With the above formulas, it is possible to compute Wp(x) from\n"
    "                 V(x) and vice-versa, using program 3dNwarpCalc.  The requisite\n"
    "                 commands are left as an exercise for the aspiring AFNI Jedi Master.\n"
    "               * Alas: -plusminus does not work with -duplo or -allineate :-(\n"
#ifdef USE_PLUSMINUS_INITIALWARP
    "               * If -plusminus is used, the -plusminus warp is initialized by\n"
    "                 a coarse warping of the source to the base, then these warp\n"
    "                 displacements are scaled by 0.5, and then the actual\n"
    "                 'meet in the middle' warp optimization begins from that point.\n"
#endif
    "               * The outputs have _PLUS (from the source dataset) and _MINUS\n"
    "                 (from the base dataset) in their filenames, in addition to\n"
    "                 the prefix.  The -iwarp option, if present, will be ignored.\n"
    "\n"
    " -pmNAMES p m = This option lets you change the PLUS and MINUS prefix appendages\n"
    "                alluded to directly above to something else that might be more\n"
    "                easy for you to grok.  For example, if you are warping EPI volumes\n"
    "                with phase-encoding in the LR-direction with volumes that had\n"
    "                phase-encoding in the RL-direction, you might do something like\n"
    "        -base EPI_LR+orig -source EPI_RL+orig -plusminus -pmNAMES RL LR -prefix EPIuw\n"
    "                recalling the the PLUS name goes with the source (RL) and the\n"
    "                MINUS name goes with the base (RL).  Then you'd end up with datasets\n"
    "                  EPIuw_LR+orig and EPIuw_LR_WARP+orig from the base\n"
    "                  EPIuw_RL+orig and EPIuw_RL_WARP+orig from the source\n"
    "                The EPIuw_LR_WARP+orig file could then be used to unwarp (e.g.,\n"
    "                using 3dNwarpApply) other LR-encoded EPI datasets from the same\n"
    "                scanning session.\n"
#endif
    "\n"
    " -nopad      = Do NOT use zero-padding on the 3D base and source images.\n"
    "               [Default == zero-pad, if needed]\n"
    "              * The underlying model for deformations goes to zero at the\n"
    "                edge of the volume being warped.  However, if there is\n"
    "                significant data near an edge of the volume, then it won't\n"
    "                get displaced much, and so the results might not be good.\n"
    "              * Zero padding is designed as a way to work around this potential\n"
    "                problem.  You should NOT need the '-nopad' option for any\n"
    "                reason that Zhark can think of, but it is here to be symmetrical\n"
    "                with 3dAllineate.\n"
    "              * Note that the output (warped from source) dataset will be on the\n"
    "                base dataset grid whether or not zero-padding is allowed.  However,\n"
    "                unless you use the following option, allowing zero-padding (i.e.,\n"
    "                the default operation) will make the output WARP dataset(s) be\n"
    "                on a larger grid.\n"
    " -nopadWARP   = If you do NOT use '-nopad' (that is, you DO allow zero-padding\n"
    "                during the warp computations), then the computed warp will often\n"
    "                be bigger than the base volume.  This situation is normally not\n"
    "                an issue, but if for some reason you require the warp volume to\n"
    "                match the base volume, then use '-nopadWARP' to have the output\n"
    "                WARP dataset(s) truncated.\n"
#ifdef USE_SAVER
    "              * Zero-padding turns off the -qsave option, since implementing\n"
    "                this combination seemed too much like work for Zhark.\n"
#endif
    "\n"
    " -verb        = Print out very very verbose progress messages (to stderr) :-)\n"
    " -quiet       = Cut out most of the fun fun fun progress messages :-(\n"
    "\n"
    "-----------------\n"
    "OUTLINE OF METHOD\n"
    "-----------------\n"
    "Repeated composition of incremental warps defined by Hermite cubic basis functions,\n"
    "first over the entire volume, then over steadily shrinking and overlapping patches\n"
    "(increasing 'levels': the patches shrink by a factor of 0.75 at each level).\n"
    "\n"
    "At 'level 0' (over the entire volume), Hermite quintic basis functions are also\n"
    "employed, but these are not used at the more refined levels.  All basis functions\n"
    "herein are (at least) continuously differentiable, so the discrete warp computed\n"
    "will be a representation of an underlying C1 diffeomorphism.  The basis functions\n"
    "go to zero at the edge of each patch, so the overall warp will decay to the identity\n"
    "warp (displacements=0) at the edge of the base volume. (However, use of '-allineate'\n"
    "can make the final output warp be nonzero at the edges; the programs that apply\n"
    "warps to datasets linearly extrapolate warp displacements outside the 3D box over\n"
    "which the warp is defined.)\n"
    "\n"
    "For this procedure to work, the source and base datasets need to be reasonably\n"
    "well aligned already (e.g., via 3dAllineate, if necessary). Multiple warps can\n"
    "later be composed and applied via programs 3dNwarpApply and/or 3dNwarpCalc.\n"
    "\n"
    "Note that it is not correct to say that the resulting warp is a piecewise cubic\n"
    "(or quintic) polynomial.  The first warp created (at level 0) is such a warp;\n"
    "call that W0(x).  Then the incremental warp W1(x) applied at the next iteration\n"
    "is also a cubic polynomial warp (say), and the result is W0(W1(x)), which is\n"
    "more complicated than a cubic polynomial -- and so on.  The incremental warps\n"
    "aren't added, but composed, so that the mathematical form of the final warp\n"
    "would be very unwieldy to express in polynomial form.  Of course, the program\n"
    "just keeps track of the displacements, not the polynomial coefficients, so it\n"
    "doesn't 'care' about the underlying polynomials at all.\n"
    "\n"
    "One reason for incremental improvement by composition, rather than by addition,\n"
    "is the simple fact that if W0(x) is invertible and W1(x) is invertible, then\n"
    "W0(W1(x)) is also invertible -- but W0(x)+W1(x) might not be.  The incremental\n"
    "polynomial warps are kept invertible by simple constraints on the magnitudes\n"
    "of their coefficients.\n"
    "\n"
    "The penalty is a Neo-Hookean elastic energy function, based on a combination\n"
    "of bulk and shear distortions; cf. http://en.wikipedia.org/wiki/Neo-Hookean_solid\n"
    "The goal is to keep the warps from becoming too 'weird' (not that this always works).\n"
    "\n"
    "By perusing the many options above, you can see that the user can control the\n"
    "warp optimization in various ways.  All these options make using 3dQwarp seem\n"
    "pretty complicated.  The reason there are so many options is that many different\n"
    "cases arise, and we are trying to makethe program flexible enough to deal with\n"
    "them all.  The SAMPLE USAGE section above is a good place to start for guidance.\n"
    "\n"
    "***** This program is experimental and subject to sudden horrific change! *****\n"
    "\n"
    "----- AUTHOR = Zhark the Grotesquely Warped -- Fall/Winter/Spring 2012-13 -----\n"
  ) ;

  PRINT_AFNI_OMP_USAGE("3dQwarp",
                       "* Tests show that using more 10-12 CPUs with 3dQwarp doesn't help.\n"
                       "  If you have more CPUs on one system, it's faster to run two or three\n"
                       "  separate registration jobs in parallel than to use all the CPUs on\n"
                       "  one 3dQwarp task.\n" ) ;
  exit(0) ;
}

/*---------------------------------------------------------------------------*/
/* Run 3dAllineate; result is stored in Qunstr.nii and Qunstr.aff12.1D */
/*---------------------------------------------------------------------------*/

static char *Qunstr=NULL ;

void Qallineate( char *basname , char *srcname , char *emkname , char *allopt )
{
   char *cmd ; int ss ;

   ss  = strlen(basname)+strlen(srcname)+2048 ;
   if( allopt  != NULL ) ss += strlen(allopt) ;
   if( emkname != NULL ) ss += strlen(emkname) ;
   cmd = (char *)malloc(ss) ;

   Qunstr = UNIQ_idcode() ;  /* create unique prefix for output filenames */

   /* setup the basic command */

   sprintf( cmd , "3dAllineate"
                  " -base %s"
                  " -source %s"
                  " -prefix %s.nii"
                  " -1Dmatrix_save %s"
                  " -cmass -final wsinc5 -float -master BASE" ,
            basname , srcname , Qunstr , Qunstr ) ;

   /* add options to the command string */

   switch( Hverb ){
     case 0: strcat(cmd," -quiet") ; break ;
     case 3:
     case 2: strcat(cmd," -verb" ) ; break ;
   }

   if( emkname != NULL )
     sprintf( cmd+strlen(cmd) , " -emask %s" , emkname) ;

   if( allopt != NULL && *allopt != '\0' )
     sprintf( cmd+strlen(cmd) , " %s"        , allopt) ;

   if( allopt == NULL || strstr(allopt,"-fineblur") == NULL )
     strcat(cmd," -fineblur 4.44") ;

#if 0
   if( allopt == NULL || strstr(allopt,"-onepass") == NULL )
     strcat(cmd," -norefinal") ;
#endif

   INFO_message("Starting 3dAllineate (affine register) command:\n  %s\n ",cmd);
   INFO_message("###########################################################") ;
   ss = system(cmd) ;
   if( ss != 0 ) ERROR_exit("3dQwarp: 3dAllineate command failed :-(") ;
   free(cmd) ;
   return ;
}

/*---------------------------------------------------------------------------*/
/* Just use 3dAllineate for resampling (no registration) */
/*---------------------------------------------------------------------------*/

void Qallin_resample( char *basname , char *srcname )  /* 17 Jul 2013 */
{
   char *cmd ; int ss ;

   ss  = strlen(basname)+strlen(srcname)+2048 ;
   cmd = (char *)malloc(ss) ;

   Qunstr = UNIQ_idcode() ;

   sprintf( cmd , "3dAllineate"
                  " -master %s"
                  " -source %s"
                  " -prefix %s.nii"
                  " -final wsinc5 -float -quiet -1Dparam_apply '1D: 12@0'\\'" ,
            basname , srcname , Qunstr ) ;

   INFO_message("Starting 3dAllineate (resample only) command:\n  %s\n ",cmd) ;
   INFO_message("###########################################################") ;
   ss = system(cmd) ;
   if( ss != 0 ) ERROR_exit("3dQwarp: 3dAllineate command failed :-(") ;
   free(cmd) ;
   return ;
}

/*---------------------------------------------------------------------------*/
/*** Main program for 3dQwarp!!! ***/
/*---------------------------------------------------------------------------*/

int main( int argc , char *argv[] )
{
   THD_3dim_dataset *bset=NULL , *sset=NULL , *oset , *iwset=NULL , *sstrue=NULL ;
   char *bsname=NULL , *iwname=NULL , *ssname=NULL , *esname=NULL ;
   MRI_IMAGE *bim=NULL , *wbim=NULL , *sim=NULL , *oim=NULL ; float bmin,smin ;
   IndexWarp3D *oww=NULL , *owwi=NULL ; Image_plus_Warp *oiw=NULL ;
   char *prefix="Qwarp" , *prefix_clean=NULL ; int nopt , nevox=0 ;
   int meth = GA_MATCH_PEARCLP_SCALAR ; int meth_is_lpc=0 ;
   int ilev = 0 , nowarp = 0 , nowarpi = 1 , mlev = 666 , nodset = 0 ;
   int duplo=0 , qsave=0 , minpatch=0 , nx,ny,nz , ct , nnn , noneg = 0 ;
   float dx,dy,dz ;
   int do_allin=0 ; char *allopt=NULL ; mat44 allin_matrix ;
   float dxal=0.0f,dyal=0.0f,dzal=0.0f ;
   int do_resam=0 ; int keep_allin=1 ;
   int flags = 0 , nbad = 0 ;
   double cput = 0.0 ;
   int do_plusminus=0; Image_plus_Warp **sbww=NULL, *qiw=NULL; /* 14 May 2013 */
   char *plusname = "PLUS" , *minusname = "MINUS" ;
   char appendage[THD_MAX_NAME] ;
   int zeropad=1, pad_xm=0,pad_xp=0, pad_ym=0,pad_yp=0, pad_zm=0,pad_zp=0 ; /* 13 Sep 2013 */
   int nxold=0,nyold=0,nzold=0 ;
   int zeropad_warp=1 ; THD_3dim_dataset *adset=NULL ;  /* 19 Mar 2014 */
   int expad=0 , minpad=0 ;
   int iwpad_xm=0,iwpad_xp=0, iwpad_ym=0,iwpad_yp=0, iwpad_zm=0,iwpad_zp=0 ;

   /*---------- enlighten the supplicant ----------*/

   AFNI_SETUP_OMP(0) ;  /* 24 Jun 2013 */

   if( argc == 1 ) { Qhelp(); exit(0); }

   /*---------- startup bureaucracy --------*/

#if defined(USING_MCW_MALLOC) && !defined(USE_OMP)
   enable_mcw_malloc() ;
#endif

   /* for OpenMP, need some workspaces for each thread */
   /* if no OpenMP, these workspaces are hard-coded in mri_nwarp.c */

#ifdef USE_OMP
   omp_set_nested(0) ;
   nthmax = omp_get_max_threads() ;              /* number of OpenMP threads */
   dhaar  = (double *)malloc(sizeof(double)*nthmax*2) ; /* make 6 workspaces */
   dhbbr  = (double *)malloc(sizeof(double)*nthmax*2) ;   /* for each thread */
   dhccr  = (double *)malloc(sizeof(double)*nthmax*2) ;
   dhddr  = (double *)malloc(sizeof(double)*nthmax*2) ;
   dheer  = (double *)malloc(sizeof(double)*nthmax*2) ;
   dhffr  = (double *)malloc(sizeof(double)*nthmax*2) ;
   INFO_message("OpenMP thread count = %d",nthmax) ;
#else
   INFO_message  ("This edition not compiled with OpenMP.") ;
   ININFO_message("It will be very slooooowwwwww .... :-(") ;
#endif

   /* standard AFNI bureaucracy */

   mainENTRY("3dQwarp") ; machdep() ;
   AFNI_logger("3dQwarp",argc,argv);
   PRINT_VERSION("3dQwarp"); AUTHOR("Zhark the (Hermite) Cubically Warped");
   (void)COX_clock_time() ;  /* initialize the clock timer */
   putenv("AFNI_WSINC5_SILENT=YES") ;

   LOAD_IDENT_MAT44(allin_matrix); /* Just to quiet initialization warning */

   /*------------- scan for and parse options -------------*/

   nopt = 1 ;
   Hblur_b = Hblur_s = 2.345f ;  /* arbitrary initializations */

   while( nopt < argc && argv[nopt][0] == '-' ){   /* loop over cmd line args */

     if( strcasecmp(argv[nopt],"-help") == 0 ||
         strcmp    (argv[nopt],"-h"   ) == 0   ){
       Qhelp(); exit(0);
     }

     if( strcasecmp(argv[nopt],"-verb") == 0 ){
       Hverb++ ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-quiet") == 0 ){
       Hverb = 0 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-nowarp") == 0 ){
       nowarp =  1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-iwarp") == 0 ){
       nowarpi = 0 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-nodset") == 0 ){
       nodset =  1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-noneg") == 0 ){  /* 24 May 2013 */
       noneg =  1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-nopad") == 0 ){  /* 13 Sep 2013 */
       zeropad = 0 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-nopadWARP") == 0 ){ /* 19 Mar 2014 */
       zeropad_warp = 0 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-expad") == 0 ){  /* SECRET OPTION */
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       expad = (int)strtod(argv[nopt],NULL) ;
       if( expad < 0 ) expad = 0 ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-minpad") == 0 ){  /* SECRET OPTION */
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       minpad = (int)strtod(argv[nopt],NULL) ;
       if( minpad < 0 ) minpad = 0 ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-allineate") == 0 ||       /* 15 Jul 2013 */
         strcasecmp(argv[nopt],"-allin"    ) == 0   ){
       do_allin =  1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-allinkeep") == 0 ){       /* 27 Aug 2013 */
       keep_allin = 1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-allinkill") == 0 ){       /* 27 Aug 2013 */
       keep_allin = 0 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-allinfast") == 0 ||       /* 19 Jul 2013 */
         strcasecmp(argv[nopt],"-allfast"  ) == 0   ){
       do_allin =  2 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-allineate_opts") == 0 ||  /* 16 Jul 2013 */
         strcasecmp(argv[nopt],"-allopt")         == 0   ){
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       allopt = strdup(argv[nopt]) ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-resample") == 0 ||
         strcasecmp(argv[nopt],"-resam"   ) == 0   ){      /* 17 Jul 2013 */
       do_resam = 1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-plusminus") == 0 || strcmp(argv[nopt],"+-") == 0 ){
#ifdef ALLOW_PLUSMINUS
       do_plusminus++ ; nopt++ ; continue ;
#else
       ERROR_exit("Option '%s' is not currently available :-(",argv[nopt]) ;
#endif
     }

     if( strcasecmp(argv[nopt],"-pmNAMES") == 0 ){
       if( ++nopt >= argc-1 ) ERROR_exit("need 2 args after %s",argv[nopt-1]) ;
       plusname = argv[nopt++] ; minusname = argv[nopt++] ; continue ;
     }

     if( strcasecmp(argv[nopt],"-nowarps") == 0 ){  /* these 2 options */
       WARNING_message("-nowarps option is now deprecated: see the -help output") ;
       nowarpi = nowarp = 1 ; nopt++ ; continue ;   /* are just for backward */
     }                                              /* compatibility */
     if( strcasecmp(argv[nopt],"-nowarpi") == 0 ){
       WARNING_message("-nowarpi option is now deprecated: see the -help output") ;
       nowarpi = 1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-patchmin") == 0 || strcasecmp(argv[nopt],"-minpatch") == 0 ){
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       minpatch = (int)strtod(argv[nopt],NULL) ;
       if( minpatch < NGMIN ) minpatch = NGMIN ; else if( minpatch%2 == 0 ) minpatch-- ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-inilev") == 0 ){
       if( duplo          ) ERROR_exit("Cannot use -inilev with -duplo :-(") ;
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       ilev = (int)strtod(argv[nopt],NULL) ;
       if( ilev < 0 ) ilev = 0 ; else if( ilev > 19 ) ilev = 19 ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-maxlev") == 0 ){
       if( duplo          ) ERROR_exit("Cannot use -maxlev with -duplo :-(") ;
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       mlev = (int)strtod(argv[nopt],NULL) ;
       if( mlev < 0 ) mlev = 0 ; else if( mlev > 99 ) mlev = 99 ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-iniwarp") == 0 ){
       char *apt=NULL ;
       if( duplo )          ERROR_exit("Cannot use -iniwarp with -duplo :-(") ;
       if( iwname != NULL ) ERROR_exit("Cannot use -iniwarp twice :-(") ;
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       iwname = strdup(argv[nopt]) ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-weight") == 0 ){  /* 17 Oct 2013 - Open Up Day */
       THD_3dim_dataset *qset ;
       if( wbim != NULL )   ERROR_exit("Cannot use -weight twice :-(") ;
       if( ++nopt >= argc ) ERROR_exit("need arg after %s",argv[nopt-1]) ;
       qset = THD_open_dataset(argv[nopt]) ;
       if( qset == NULL )   ERROR_exit("Cannot open -weight dataset :-(") ;
       DSET_load(qset) ; CHECK_LOAD_ERROR(qset) ;
       wbim = THD_extract_float_brick(0,qset) ; DSET_delete(qset) ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-duplo") == 0 ){
       if( iwname != NULL )
         ERROR_exit("Cannot use -duplo with -iniwarp :-(") ;
       if( ilev != 0 || mlev < 99 )
         ERROR_exit("Cannot use -duplo with -inilev or -maxlev :-(") ;
       duplo = 1 ; nopt++ ; continue ;
     }

     if( strncasecmp(argv[nopt],"-workhard",9) == 0 ){
       char *wpt = argv[nopt]+9 ;
       Hworkhard1 = 0 ; Hworkhard2 = 66 ;
       if( *wpt == ':' && isdigit(*(wpt+1)) ){
         char *cpt ;
         Hworkhard2 = (int)strtod(++wpt,NULL) ;
         cpt = strchr(wpt,':') ;
         if( cpt != NULL && isdigit(*(cpt+1)) ){
           Hworkhard1 = Hworkhard2 ;
           Hworkhard2 = (int)strtod(++cpt,NULL) ;
         }
       }
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-zeasy") == 0 ){     /* 26 Jun 2014 */
       Hzeasy = 1 ; nopt++ ; continue ;
     }
     if( strcasecmp(argv[nopt],"-noQ") == 0 ){       /* 01 Jul 2014 */
       Hznoq = 1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-superhard") == 0 ){  /* 30 Apr 2013 */
       char *wpt = argv[nopt]+9 ;
       Hsuperhard1 = 0 ; Hsuperhard2 = 66 ;
       if( *wpt == ':' && isdigit(*(wpt+1)) ){
         char *cpt ;
         Hsuperhard2 = (int)strtod(++wpt,NULL) ;
         cpt = strchr(wpt,':') ;
         if( cpt != NULL && isdigit(*(cpt+1)) ){
           Hsuperhard1 = Hsuperhard2 ;
           Hsuperhard2 = (int)strtod(++cpt,NULL) ;
         }
       }
       nopt++ ; continue ;
     }

#ifdef ALLOW_QMODE
     if( strcasecmp(argv[nopt],"-Qfinal") == 0 ){     /* 07 May 2013 */
       Hqfinal = 1 ; nopt++ ; continue ;
     }
     if( strcasecmp(argv[nopt],"-Qonly") == 0 ){      /* 27 Jun 2013 */
       Hqonly = 1 ; nopt++ ; continue ;
     }
#endif

     if( strcasecmp(argv[nopt],"-qsave") == 0 ){
#ifndef USE_SAVER
       WARNING_message("-qsave option is not compiled into this copy of 3dQwarp :-(") ;
#else
       qsave = 1 ;
#endif
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-noXdis") == 0 ){
       flags |= NWARP_NOXDIS_FLAG ; nopt++ ; continue ;
     }
     if( strcasecmp(argv[nopt],"-noYdis") == 0 ){
       flags |= NWARP_NOYDIS_FLAG ; nopt++ ; continue ;
     }
     if( strcasecmp(argv[nopt],"-noZdis") == 0 ){
       flags |= NWARP_NOZDIS_FLAG ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-base") == 0 ){
       if( bset   != NULL ) ERROR_exit("Can't use -base twice!") ;
       if( ++nopt >= argc ) ERROR_exit("need arg after -base") ;
       bset = THD_open_dataset(argv[nopt]) ; if( bset == NULL ) ERROR_exit("Can't open -base") ;
       bsname = strdup(argv[nopt]) ; DSET_COPYOVER_REAL(bset) ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-source") == 0 ){
       if( sset   != NULL ) ERROR_exit("Can't use -source twice!") ;
       if( ++nopt >= argc ) ERROR_exit("need arg after -source") ;
       sset = THD_open_dataset(argv[nopt]) ; if( sset == NULL ) ERROR_exit("Can't open -source") ;
       ssname = strdup(argv[nopt]) ; sstrue = sset ; DSET_COPYOVER_REAL(sset) ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-emask") == 0 ){
       THD_3dim_dataset *eset ;
       if( Hemask != NULL ) ERROR_exit("Can't use -emask twice!") ;
       if( ++nopt >= argc ) ERROR_exit("need arg after -emask") ;
       eset = THD_open_dataset(argv[nopt]) ; if( eset == NULL ) ERROR_exit("Can't open -emask") ;
       DSET_load(eset) ; CHECK_LOAD_ERROR(eset) ;
       Hemask = THD_makemask( eset , 0 , 1.0f , -1.0f ) ;
       if( Hemask == NULL ) ERROR_exit("Can't make -emask for some reason :-(") ;
       nevox = DSET_NVOX(eset) ;
       DSET_delete(eset) ; esname = strdup(argv[nopt]) ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-blur") == 0 ){
       float val1,val2 ;
       if( ++nopt >= argc ) ERROR_exit("need arg after -blur") ;
       if( !isnumeric(argv[nopt][0]) )
         ERROR_exit("value after '-blur' must be a number: '%s'",argv[nopt]) ;
       val2 = val1 = (float)strtod(argv[nopt],NULL) ;
       if( nopt+1 < argc && isnumeric(argv[nopt+1][0]) && !isalpha(argv[nopt+1][1]) )
         val2 = (float)strtod(argv[++nopt],NULL) ;
       Hblur_b = val1 ; Hblur_s = val2 ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-nopenalty") == 0 ){
       Hpen_fac = 0.0 ; nopt++ ; continue ;
     }

     if( strncasecmp(argv[nopt],"-useweight",10) == 0 ||
         strcasecmp (argv[nopt],"-use_weight"  ) == 0   ){
       auto_weight = 1 ;
       if( argv[nopt][10] == '*' && argv[nopt][11] == '*' && isnumeric(argv[nopt][12]) )
         auto_wpow = (float)strtod(argv[nopt]+12,NULL) ;
       if( Hverb && auto_wpow != 1.0f ) INFO_message("-useweight is now the default") ;
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-noweight") == 0 ){
       auto_weight = 2 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-penfac") == 0 ){
       double val ;
       if( ++nopt >= argc ) ERROR_exit("need arg after -penfac") ;
       val = strtod(argv[nopt],NULL) ;
       if( val <= 0.0 ){
         INFO_message("-penfac turns the penalty off") ;  Hpen_fac = 0.0 ;
       } else {
         Hpen_fac = Hpen_fbase * val ;
       }
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-pencut") == 0 ){  /* 21 Mar 2014 [SECRET OPTION] */
       double val ;
       if( ++nopt >= argc ) ERROR_exit("need arg after -pencut") ;
       val = strtod(argv[nopt],NULL) ;
       if( val < 0.0 || val > 10.0 ){
         INFO_message("-pencut %g is illegal -- replaced with 1.0",val) ;  Hpen_cut = 1.0f ;
       } else if( val > 1.0f ){
         Hpen_cut = (float)(0.1*val) ;
       } else {
         Hpen_cut = (float)val ;
       }
       nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-penold") == 0 ){
       Hpen_old = 1 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-prefix") == 0 ){
       if( ++nopt >= argc ) ERROR_exit("need arg after -prefix") ;
       prefix = strdup(argv[nopt]) ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-hel") == 0 ){
       meth = GA_MATCH_HELLINGER_SCALAR ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-mi") == 0 ){
       meth = GA_MATCH_KULLBACK_SCALAR ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-nmi") == 0 ){
       meth = GA_MATCH_NORMUTIN_SCALAR ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-pcl") == 0 ){
       meth = GA_MATCH_PEARCLP_SCALAR ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-pear") == 0 ){
       meth = GA_MATCH_PEARSON_SCALAR ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-lpc") == 0 ){
       meth = GA_MATCH_PEARSON_LOCALS ;
       Hzeasy = meth_is_lpc = 1 ; mlev = 0 ; nopt++ ; continue ;
     }

     if( strcasecmp(argv[nopt],"-lpa") == 0 ){
       meth = GA_MATCH_PEARSON_LOCALA ; nopt++ ; continue ;
     }

#if 0  /* this should NOT be enabled! */
     if( strcasecmp(argv[nopt],"-localstat") == 0 ){  /* 09 Sep 2013 */
       Hlocalstat = 1 ; nopt++ ; continue ;
     }
#endif

     /*--- maybe we should just tell them to use SPM? ---*/

     ERROR_message("Totally bogus option '%s'",argv[nopt]) ;
     suggest_best_prog_option(argv[0], argv[nopt]) ;
     exit(1);

   } /*----- end of loop over command line args -----*/

   if( argc < 3 )
     ERROR_exit("Too few options, use -help for details");

   /*----- make a 'clean' prefix -----*/

   { char *ns ;
     prefix_clean = strdup(prefix) ;
     ns = strstr(prefix_clean,".nii" ) ; if( ns != NULL ) *ns = '\0' ;
     ns = strstr(prefix_clean,"+orig") ; if( ns != NULL ) *ns = '\0' ;
     ns = strstr(prefix_clean,"+acpc") ; if( ns != NULL ) *ns = '\0' ;
     ns = strstr(prefix_clean,"+tlrc") ; if( ns != NULL ) *ns = '\0' ;
   }

   /*----- check for errorororors --------------------------------------------*/

STATUS("check for errors") ;

   nbad = 0 ;                 /* count of errors */
   ct   = NI_clock_time() ;   /* Oh, and start the clock timer */

   if( flags == NWARP_NODISP_FLAG ){
     ERROR_message("too many -no?dis flags ==> nothing to warp!") ; nbad++ ;
   }

   if( bset == NULL && sset == NULL && nopt+1 >= argc ){
     ERROR_message("need 2 args after all options, for base and source dataset names") ; nbad++ ;
   }

   if( do_allin && do_resam ){
     do_resam = 0 ;
     INFO_message("%s turns off -resample",(do_allin==1)?"-allineate":"-allinfast");
   }
   if( do_allin && do_plusminus ){
     ERROR_message("You cannot use -allineate and -plusminus together :-(") ; nbad++ ;
   }
   if( do_allin && iwname != NULL ){
     ERROR_message("You cannot use -allineate and -iniwarp together :-(") ; nbad++ ;
   }
   if( do_allin && ilev > 0 ){
     ERROR_message("You cannot use -allineate and -inilev together :-(") ; nbad++ ;
   }

   if( iwname != NULL && duplo ){
     ERROR_message("You cannot combine -iniwarp and -duplo !! :-((") ; nbad++ ;
   }
   if( (ilev != 0 || mlev < 99 ) && duplo ){
     ERROR_message("You cannot combine -inilev or -maxlev and -duplo !! :-((") ; nbad++ ;
   }

   if( bset != NULL && sset == NULL ){
     ERROR_message("You can't use -base without -source!") ; nbad++ ;
   } else if( bset == NULL && sset != NULL ) {
     ERROR_message("You can't use -source without -base!") ; nbad++ ;
   }

   /*--- any fatal errors flagged above, then it's time to go fatal ---*/

   if( nbad > 0 )
     ERROR_exit("Cannot continue after above error%s" , (nbad==1) ? "\0" : "s" ) ;

   /*--- other checks that aren't fatal, just to let the user beware ---*/

   if( meth_is_lpc && mlev > 0 )
     WARNING_message("Use of '-maxlev 0' is recommended with '-lpc'") ;

   if( do_plusminus && duplo ){
     duplo = 0 ;
     WARNING_message("Alas, -plusminus does not work with -duplo -- turning -duplo off") ;
   }

   if( Hznoq && Hqonly ){
     Hznoq = 0 ;
     WARNING_message("-znoQ and -Qonly cannot be combined: turning off -znoQ") ;
   } else if( Hznoq && Hqfinal ){
     Hznoq = 0 ;
     WARNING_message("-znoQ and -Qfinal cannot be combined: turning off -znoQ") ;
   }

#if 0
   if( Hlocalstat && meth != GA_MATCH_PEARCLP_SCALAR && meth != GA_MATCH_PEARSON_SCALAR ){
     Hlocalstat = 0 ;
     WARNING_message("the cost functional choice disables -localstat") ;
   }
#endif

   /*----------- get the input datasts, check them for errors -----------*/

STATUS("read inputs") ;

   /* base dataset (if not already read) */

   if( bset == NULL ){
     bset = THD_open_dataset(argv[nopt++]) ; DSET_COPYOVER_REAL(bset) ;
     if( bset == NULL ) ERROR_exit("Can't open base dataset") ;
     bsname = strdup(argv[nopt-1]) ;
STATUS("base dataset opened") ;
   }
   if( DSET_NVALS(bset) > 1 )
     INFO_message("base dataset has more than 1 sub-brick: ignoring all but the first") ;

   /* source dataset (if not already read) */

   if( sset == NULL ){
     sset = THD_open_dataset(argv[nopt++]) ; DSET_COPYOVER_REAL(sset) ;
     if( sset == NULL ) ERROR_exit("Can't open source dataset") ;
     ssname = strdup(argv[nopt-1]) ; sstrue = sset ;
STATUS("source dataset opened") ;
   }
   if( DSET_NVALS(sset) > 1 )
     INFO_message("source dataset has more than 1 sub-brick: ignoring all but the first") ;

   if( do_resam && EQUIV_GRIDXYZ(bset,sset) ){
     INFO_message("-resample is not needed (datasets on same 3D grid) -- turning it off") ;
     do_resam = 0 ;
   }

   /*---- Run 3dAllineate first, replace source dataset [15 Jul 2013] --------*/

   if( !do_allin && allopt != NULL ){
     WARNING_message("-allineate_opts is ignored: no -allineate option was given!") ;
     free(allopt) ; allopt = NULL ;
   }

   if( do_allin || do_resam ){   /* here is where 3dAllineate is invoked */
     char *qs , *ns , *rs ; /* temp stuff */

STATUS("3dAllineate coming up next") ;

     if( do_allin && noneg ){
       if( allopt != NULL ) allopt = (char *)realloc(allopt,strlen(allopt)+32);
       else                 allopt = (char *)calloc(32,1) ;
       strcat(allopt," -zclip") ;
     }
     if( do_allin == 2 ){  /* the 'fast' way */
       if( allopt != NULL ) allopt = (char *)realloc(allopt,strlen(allopt)+64);
       else                 allopt = (char *)calloc(64,1) ;
       strcat(allopt," -norefinal -onepass -conv 1.0") ;
     }

     DSET_unload(sstrue) ;                /* de-allocate orig source dataset */

     /*--- run 3dAllineate now now now ---*/

     if( do_allin )
       Qallineate( bsname , ssname , esname , allopt ) ;  /* run 3dAllineate */
     else /* do_resam */
       Qallin_resample( bsname , ssname ) ;           /* just for resampling */

     fprintf(stderr,"\n") ;

     /*-- try to load the results: dataset first (in NIfTI format) --*/

     qs = (char *)malloc(strlen(Qunstr)+strlen(prefix)+64) ;
     ns = (char *)malloc(strlen(Qunstr)+strlen(prefix)+64) ;
     sprintf(qs,"%s.nii",Qunstr) ;                        /* dataset filename */
     if( !THD_is_file(qs) ){     /* check for compressed output [07 Feb 2014] */
       strcpy(ns,qs) ; strcat(ns,".gz") ;
       if( !THD_is_file(ns) )
         ERROR_message("Can't find 3dAllineate output '%s' or '%s' :-(",qs,ns) ;
       else
         strcpy(qs,ns) ;
     }
     if( keep_allin ){    /* if keeping 3dAllineate output, make a nicer name */
       sprintf(ns,"%s_Allin.nii",prefix_clean) ;
       if( STRING_HAS_SUFFIX(qs,".gz") ) strcat(ns,".gz") ;    /* 07 Feb 2014 */
       rename(qs,ns) ; rs = ns ;
     } else {
       rs = qs ;
     }
     INFO_message("3dQwarp: replacing source dataset with 3dAllineate result %s",rs) ;
     sset = THD_open_dataset(rs) ;                 /* get its output dataset */
     if( sset == NULL ) ERROR_exit("Can't open replacement source dataset %s :-(",rs) ;
     DSET_load(sset); CHECK_LOAD_ERROR(sset); DSET_lock(sset); DSET_COPYOVER_REAL(sset);
     if( !keep_allin ) remove(qs) ;  /* erase the 3dAllineate dataset from disk */

     /*-- load alignment matrix, to be used after warping --*/

     if( do_allin ){
       MRI_IMAGE *zim ; float *qar ;
       sprintf(qs,"%s.aff12.1D",Qunstr) ;
       zim = mri_read_1D(qs) ;           /* get the 3dAllineate output matrix */
       if( zim == NULL )
         ERROR_exit("Can't open 3dAllineate's .aff12.1D file??") ;
       if( zim->nvox < 12 )
         ERROR_exit("3dAllineate's .aff12.1D file has incorrect format??") ;
       qar = MRI_FLOAT_PTR(zim) ;
       LOAD_MAT44(allin_matrix,qar[0],qar[1],qar[ 2],qar[ 3],  /* load matrix */
                               qar[4],qar[5],qar[ 6],qar[ 7],
                               qar[8],qar[9],qar[10],qar[11] ) ;
         /* save the magnitude of the shifts (needed for zero pad guesstimate */
       dxal = fabsf(qar[3]) ; dyal = fabsf(qar[7]) ; dzal = fabsf(qar[11]) ;
       if( !keep_allin ){
         remove(qs) ;           /* erase the 3dAllineate matrix file from disk */
         if( Hverb ) ININFO_message("3dAllineate output files have been deleted");
       } else {
         sprintf(ns,"%s_Allin.aff12.1D",prefix_clean) ; rename(qs,ns) ;
         if( Hverb ) ININFO_message("3dAllineate output files have been renamed") ;
       }
       if( Hverb && do_allin ) DUMP_MAT44("3dAllineate matrix",allin_matrix) ;
       mri_free(zim) ;
     }

     free(qs) ; free(ns) ;

   } /*--- end of 3dAllineate prolegomenon -----------------------------------*/

STATUS("check dataset for stupid errors") ; /*--------------------------------*/

   if( !EQUIV_GRIDXYZ(bset,sset) )
     ERROR_exit("base-source dataset grid mismatch :-( : try the -resample option") ;

   if(  EQUIV_DSETS  (bset,sset) )
     ERROR_exit("base & source datasets are identical :-( : are you trying something sneaky?");

   /*--------------- construct the initial warp dataset, if any
                     [altered somewhat: 15 Jul 2013]            --------------*/

   if( iwname != NULL ){
     int ijkpad[12] , qq ; THD_3dim_dataset *qsar[2] ;

STATUS("construct initial warp") ;

     /* take care of case when just a .1D file is input here */

     if( strstr(iwname,".1D") != NULL && strchr(iwname,' ') == NULL ){
       char *qstr = (char *)malloc(strlen(iwname)+strlen(bsname)+64) ;
       sprintf(qstr,"IDENT(%s) %s",bsname,iwname) ;
       free(iwname) ; iwname = qstr ;
     }

     iwset = IW3D_read_catenated_warp(iwname) ;    /* fancy way to read warps */
     if( iwset == NULL )
       ERROR_exit("Cannot open -iniwarp %s",iwname) ;
     if( DSET_NVALS(iwset) < 3 || DSET_BRICK_TYPE(iwset,0) != MRI_float )
       ERROR_exit("-iniwarp %s is not in the right format :-(",argv[nopt]) ;

     /* compute how much bset must be padded to fit the iniwarp [11 Apr 2014] */

     qsar[0] = iwset ; qsar[1] = bset ;
     qq = THD_conformist( 2 , qsar , CONFORM_NOREFIT , ijkpad ) ;
     if( qq < 0 )
       ERROR_exit("-iniwarp grid does not conform with base dataset grid") ;
     if( ijkpad[0] > 0 || ijkpad[1] > 0 || ijkpad[2] > 0 ||
         ijkpad[3] > 0 || ijkpad[4] > 0 || ijkpad[5] > 0   )
       ERROR_exit("-iniwarp grid conforms to but does not contain base dataset grid") ;

     iwpad_xm=ijkpad[6]; iwpad_xp=ijkpad[ 7]; iwpad_ym=ijkpad[ 8];
     iwpad_yp=ijkpad[9]; iwpad_zm=ijkpad[10]; iwpad_zp=ijkpad[11];
     if( !zeropad &&
         (iwpad_xm > 0 || iwpad_xp > 0 || iwpad_ym > 0 ||
          iwpad_yp > 0 || iwpad_zm > 0 || iwpad_zp > 0   ) )
       ERROR_exit("-iniwarp grid is bigger than base dataset grid AND you used -nopad") ;

     if( Hverb > 1 &&
         (iwpad_xm > 0 || iwpad_xp > 0 || iwpad_ym > 0 ||
          iwpad_yp > 0 || iwpad_zm > 0 || iwpad_zp > 0   ) )
       INFO_message("-iniwarp requires dataset to be padded at least %d %d  %d %d  %d %d voxels",
                    iwpad_xm, iwpad_xp, iwpad_ym, iwpad_yp, iwpad_zm, iwpad_zp ) ;

   } /* at this point, iwset = initial warp dataset */

STATUS("load datasets") ; /*--------------------------------------------------*/

   DSET_load(bset) ; CHECK_LOAD_ERROR(bset) ;
   bim = THD_extract_float_brick(0,bset) ; DSET_unload(bset) ;
   if( DSET_NVALS(bset) > 1 )
     INFO_message("base dataset has more than 1 sub-brick: ignoring all but the first") ;

   DSET_load(sset) ; CHECK_LOAD_ERROR(sset) ;
   sim = THD_extract_float_brick(0,sset) ;
   DSET_unlock(sset) ; DSET_unload(sset) ;

   if( nevox > 0 && nevox != DSET_NVOX(bset) )
     ERROR_exit("-emask doesn't match base dataset grid :-(") ;

   /*---------------- deal with negative values [24 May 2013] ----------------*/

   bmin = mri_min(bim) ;
   if( bmin < 0.0f && noneg ){
     float *bar = MRI_FLOAT_PTR(bim) ; int ii , nneg=0 ;
     for( ii=0 ; ii < bim->nvox ; ii++ ){ if( bar[ii] < 0.0f ){ bar[ii] = 0.0; nneg++; } }
     INFO_message("-noneg converted %d base voxels to 0",nneg) ; bmin = 0.0f ;
   }

   smin = mri_min(sim) ;
   if( smin < 0.0f && noneg ){
     float *sar = MRI_FLOAT_PTR(sim) ; int ii , nneg=0 ;
     for( ii=0 ; ii < sim->nvox ; ii++ ){ if( sar[ii] < 0.0f ){ sar[ii] = 0.0; nneg++; } }
     INFO_message("-noneg converted %d source voxels to 0",nneg) ; smin = 0.0f ;
   }

   if( (bmin < 0.0f || smin < 0.0f) && meth == GA_MATCH_PEARCLP_SCALAR ){
     meth = GA_MATCH_PEARSON_SCALAR ;
     INFO_message("negative values in %s ==> using strict Pearson correlation",
                     (bmin < 0.0f && smin < 0.0f) ? "base and source"
                   : (bmin < 0.0f)                ? "base"            : "source" ) ;
   }

   /*----------- dimensions of the universe (as it is now, anyhoo) -----------*/

   nxold = nx = DSET_NX(bset); nyold = ny = DSET_NY(bset); nzold = nz = DSET_NZ(bset);
   dx = fabsf(DSET_DX(bset)) ; dy = fabsf(DSET_DY(bset)) ; dz = fabsf(DSET_DZ(bset)) ;

   /*------ Do we need to zeropad datasets? [Friday the 13th, Sep 2013] ------*/
   /*(((((( That is, do we expand the universe with dark matter/energy? ))))))*/

   if( expad > 0 || minpad > 0 ) zeropad = 1 ;

   if( zeropad ){                /* adapted/stolen/liberated from 3dAllineate */
     float cv , *qar  ; MRI_IMAGE *qim ; int mpad_min=9 ;
     int bpad_xm,bpad_xp, bpad_ym,bpad_yp, bpad_zm,bpad_zp ;
     int spad_xm,spad_xp, spad_ym,spad_yp, spad_zm,spad_zp ;
     int mpad_x , mpad_y , mpad_z , ii ;

     cv = 0.33f * THD_cliplevel(bim,0.22f) ;        /* set threshold on base, */
     qim = mri_copy(bim); qar = MRI_FLOAT_PTR(qim); /* then pad on bounding box */
     for( ii=0 ; ii < qim->nvox ; ii++ ) if( qar[ii] < cv ) qar[ii] = 0.0f ;
     MRI_autobbox( qim, &bpad_xm,&bpad_xp, &bpad_ym,&bpad_yp, &bpad_zm,&bpad_zp ) ;
     mri_free(qim) ;
     if( Hverb > 1 )
       INFO_message("Zero-pad: base dataset autobox = %d..%d  %d..%d  %d..%d",
                    bpad_xm,bpad_xp , bpad_ym,bpad_yp , bpad_zm,bpad_zp ) ;

     /* the code to use the source to define the zero pad is removed,
        so that the zero padding is the same whenever the same base is used */

#if 0  /* use the source to define zero pad amount */
     cv = 0.33f * THD_cliplevel(sim,0.22f) ;       /* set threshold on source */
     qim = mri_copy(sim); qar = MRI_FLOAT_PTR(qim);
     for( ii=0 ; ii < qim->nvox ; ii++ ) if( qar[ii] < cv ) qar[ii] = 0.0f ;
     MRI_autobbox( qim, &spad_xm,&spad_xp, &spad_ym,&spad_yp, &spad_zm,&spad_zp ) ;
     mri_free(qim) ;
     if( Hverb > 1 )
       ININFO_message("Zero-pad: source dataset autobox = %d..%d  %d..%d  %d..%d",
                      bpad_xm,bpad_xp , bpad_ym,bpad_yp , bpad_zm,bpad_zp ) ;
#else  /* do NOT use the source to define zero pad amount */
     spad_xm = bpad_xm ; spad_ym = bpad_ym ; spad_zm = bpad_zm ;
     spad_xp = bpad_xp ; spad_yp = bpad_yp ; spad_zp = bpad_zp ;
#endif

     /* these variables are the box in which the data is contained */

     pad_xm = MIN(bpad_xm,spad_xm) ; pad_xp = MAX(bpad_xp,spad_xp) ;
     pad_ym = MIN(bpad_ym,spad_ym) ; pad_yp = MAX(bpad_yp,spad_yp) ;
     pad_zm = MIN(bpad_zm,spad_zm) ; pad_zp = MAX(bpad_zp,spad_zp) ;

     if( do_allin ){                              /* extend pad size for */
       float dm = MIN(dx,dy) ; dm = MIN(dm,dz) ;  /* 3dAllineate shifts? */
       dxal /= dm ; dyal /= dm ; dzal /= dm ;
       dm = MAX(dxal,dyal) ; dm = MAX(dm,dzal) ; mpad_min += (int)rintf(1.0111f*dm) ;
     }

     /* define minimum padding for each direction */

     mpad_x = (int)rintf(0.1111f*bim->nx) ; mpad_x = MAX(mpad_x,mpad_min) ;
     mpad_y = (int)rintf(0.1111f*bim->ny) ; mpad_y = MAX(mpad_y,mpad_min) ;
     mpad_z = (int)rintf(0.1111f*bim->nz) ; mpad_z = MAX(mpad_z,mpad_min) ;

     /* compute padding so at least mpad_Q all-zero slices on each Q-face
        will be present after the padding is done, for Q = x or y or z   */

     pad_xm = mpad_x - pad_xm               ; if( pad_xm < 0 ) pad_xm = 0 ;
     pad_ym = mpad_y - pad_ym               ; if( pad_ym < 0 ) pad_ym = 0 ;
     pad_zm = mpad_z - pad_zm               ; if( pad_zm < 0 ) pad_zm = 0 ;
     pad_xp = mpad_x - (bim->nx-1 - pad_xp) ; if( pad_xp < 0 ) pad_xp = 0 ;
     pad_yp = mpad_y - (bim->ny-1 - pad_yp) ; if( pad_yp < 0 ) pad_yp = 0 ;
     pad_zp = mpad_z - (bim->nz-1 - pad_zp) ; if( pad_zp < 0 ) pad_zp = 0 ;

     if( Hverb > 1 &&
         (pad_xm > 0 || pad_xp > 0 || pad_ym > 0 ||
          pad_yp > 0 || pad_zm > 0 || pad_zp > 0   ) )
       ININFO_message("dataset padding needs at least %d %d  %d %d  %d %d voxels",
                      pad_xm, pad_xp, pad_ym, pad_yp, pad_zm, pad_zp ) ;

     if( pad_xm < iwpad_xm ) pad_xm = iwpad_xm ;  /* make sure padding */
     if( pad_xp < iwpad_xp ) pad_xp = iwpad_xp ;  /* is at least what */
     if( pad_ym < iwpad_ym ) pad_ym = iwpad_ym ;  /* is needed to fit */
     if( pad_yp < iwpad_yp ) pad_yp = iwpad_yp ;  /* the initial warp */
     if( pad_zm < iwpad_zm ) pad_zm = iwpad_zm ;
     if( pad_zp < iwpad_zp ) pad_zp = iwpad_zp ;

     if( pad_xm < minpad   ) pad_xm = minpad ;  /* minimum padding allowed? */
     if( pad_xp < minpad   ) pad_xp = minpad ;  /* (SECRET OPTION) */
     if( pad_ym < minpad   ) pad_ym = minpad ;
     if( pad_yp < minpad   ) pad_yp = minpad ;
     if( pad_zm < minpad   ) pad_zm = minpad ;
     if( pad_zp < minpad   ) pad_zp = minpad ;

     if( expad > 0 ){                             /* extra padding   */
       pad_xm += expad ; pad_xp += expad ;        /* ordered by the  */
       pad_ym += expad ; pad_yp += expad ;        /* cautious user   */
       pad_zm += expad ; pad_zp += expad ;        /* (SECRET OPTION) */
     }

     if( bim->nz == 1 ){     /* but no z-padding for 2D image! */
       pad_zm = pad_zp = 0 ;
       if( iwpad_zm > 0 || iwpad_zp > 0 )
         ERROR_exit("-iniwarp required padding in 3D but base dataset is 2D ?!?") ;
     }

     /* flag to mark if any zero padding is done */

     zeropad = (pad_xm > 0 || pad_xp > 0 ||
                pad_ym > 0 || pad_yp > 0 || pad_zm > 0 || pad_zp > 0) ;

#ifdef USE_SAVER
     if( zeropad && qsave ){  /* too much trouble to do both */
       INFO_message("-qsave is turned off because zero-padding is happening!") ;
       qsave = 0 ;
     }
#endif

     if( zeropad ){  /*----- print a report and actually do it -----*/
       if( Hverb )
         INFO_message("Dataset zero-pad: xbot=%d xtop=%d  ybot=%d ytop=%d  zbot=%d ztop=%d voxels",
                       pad_xm,pad_xp , pad_ym,pad_yp , pad_zm,pad_zp ) ;

       /*-- replace base image --*/

       nxold=bim->nx ; nyold=bim->ny ; nzold=bim->nz ;  /* orginal dimensions */

       qim = mri_zeropad_3D( pad_xm,pad_xp , pad_ym,pad_yp ,
                                             pad_zm,pad_zp , bim ) ;
       mri_free(bim) ; bim = qim ;

       /*-- replace source image --*/

       qim = mri_zeropad_3D( pad_xm,pad_xp , pad_ym,pad_yp ,
                                             pad_zm,pad_zp , sim ) ;
       mri_free(sim) ; sim = qim ;

       /*-- also zeropad emask --*/

       if( Hemask != NULL ){
         byte *ezp = (byte *)EDIT_volpad( pad_xm,pad_xp ,
                                          pad_ym,pad_yp ,
                                          pad_zm,pad_zp ,
                                          nxold,nyold,nzold ,
                                          MRI_byte , Hemask ) ;
         if( ezp == NULL ) ERROR_exit("zeropad of emask fails !?!") ;
         free(Hemask) ; Hemask = ezp ; nevox = bim->nvox ;
       }
     }

     nx = bim->nx; ny = bim->ny; nz = bim->nz; /* altered dimensions of universe */

   } /*--------- end of zeropad for base, source, and emask inputs ---------*/

#if 0
   if( !zeropad ) zeropad_warp = 0 ;
#endif

   /*------- setup initial warp, if any (e.g., make it match bset) -------*/
   /*((((((( Global variable S2BIM_iwarp contains the initial warp )))))))*/

   if( iwset != NULL ){

     int nxiw,nyiw,nziw , mm ;
     DSET_load(iwset) ; CHECK_LOAD_ERROR(iwset) ;

     /* Compute how much iwset must be padded to fit bset [11 Apr 2014] */
     /* Remember, at this point, bset might be zeropaded to be bigger.  */
     /* By construction above, iwpad_?? <= pad_?? for all 6 ?? values,  */
     /* so we need to pad the initial warp by pad_?? - iwpad_??         */

     iwpad_xm = pad_xm - iwpad_xm ; iwpad_xp = pad_xp - iwpad_xp ;
     iwpad_ym = pad_ym - iwpad_ym ; iwpad_yp = pad_yp - iwpad_yp ;
     iwpad_zm = pad_zm - iwpad_zm ; iwpad_zp = pad_zp - iwpad_zp ;

     /* convert input dataset to index warp */

     S2BIM_iwarp = IW3D_from_dataset(iwset,0,0) ;
     if( S2BIM_iwarp == NULL )
       ERROR_exit("Cannot create 3D warp from -iniwarp dataset :-(") ;
     DSET_delete(iwset) ; iwset = NULL ;

     /* pad (actually, extend in size by linear extrapolation), if needed */

     if( iwpad_xm > 0 || iwpad_xp > 0 || iwpad_ym > 0 ||
         iwpad_yp > 0 || iwpad_zm > 0 || iwpad_zp > 0   ){

       IndexWarp3D *QQ = IW3D_extend(S2BIM_iwarp, iwpad_xm,iwpad_xp,
                                                  iwpad_ym,iwpad_yp, iwpad_zm,iwpad_zp , 0 ) ;
       IW3D_destroy(S2BIM_iwarp) ; S2BIM_iwarp = QQ ;
       if( Hverb )
         ININFO_message("Extended/padded iniwarp to match base volume: %d %d  %d %d  %d %d voxels",
                        iwpad_xm,iwpad_xp, iwpad_ym,iwpad_yp, iwpad_zm,iwpad_zp ) ;

     }
#if 0
     if( Hverb > 1 )
       INFO_message("-iniwarp energy = %g",IW3D_load_energy(S2BIM_iwarp)) ;
#endif

   } else {  /*------- no initial warp (that was easy) -----------------------*/

     S2BIM_iwarp = NULL ;

   }

   /*----------------------- other initial setup stuff -----------------------*/

   S2BIM_ilev = ilev ;            /* initial level */
   S2BIM_mlev = MAX(mlev,ilev) ;  /* maximum level */

   nnn = 0 ;
   if( nx >= NGMIN             ) nnn = nx ;  /* find largest dimension */
   if( ny >= NGMIN && ny > nnn ) nnn = ny ;  /* bigger than min grid  */
   if( nz >= NGMIN && nz > nnn ) nnn = nz ;  /* allowed for warping  */
   if( nnn == 0 )
     ERROR_exit("dataset grid size %d x %d x %d is too small for warping",nx,ny,nz) ;

#if 0
   if( minpatch > 0 && minpatch > nnn/2 && minpatch > NGMIN ){
     WARNING_message("-minpatch %d is too large for dataset grid %d x %d x %d",
                     minpatch , nx,ny,nz ) ;
     minpatch = nnn/2 ;
     if( minpatch%2 == 0     ) minpatch-- ;
     if( minpatch   <  NGMIN ) minpatch = NGMIN ;
     WARNING_message("minpatch is reduced to %d",minpatch) ;
   }
#endif

   if( minpatch > 0 ) Hngmin = minpatch ;  /* minimum patch size */

   if( duplo && (nx < 3*Hngmin || ny < 3*Hngmin || nz < 3*Hngmin) ){
     duplo = 0 ;
       INFO_message("-duplo disabled since dataset is small: %d x %d x %d",nx,ny,nz) ;
     ININFO_message(" smallest size allowed for -duplo is    %d x %d x %d",3*Hngmin,3*Hngmin,3*Hngmin) ;
     ININFO_message(" ['small' is relative to the minimum patch size you set = %d]",Hngmin) ;
   }

#ifdef USE_SAVER  /* doesn't work, don't do this! */
   if( qsave ){
     qset = EDIT_empty_copy(bset) ;
     EDIT_dset_items( qset ,
                        ADN_prefix    , modify_afni_prefix(prefix,NULL,"_SAVE") ,
                        ADN_nvals     , 1 ,
                        ADN_ntt       , 0 ,
                        ADN_datum_all , MRI_float ,
                      ADN_none ) ;
     EDIT_BRICK_FACTOR(qset,0,0.0) ;
     iterfun = Qsaver ;
     tross_Copy_History( bset , qset ) ;
     tross_Make_History( "3dQwarp" , argc,argv , qset ) ;
   }
#endif

   /*-------------------- create weight volume -------------------------------*/

STATUS("construct weight/mask volume") ;

   if( wbim == NULL ){  /* construct from base image */

     wbim = mri_weightize(bim,auto_weight,auto_dilation,auto_wclip,auto_wpow) ;

   } else {             /* just read user input */

     if( zeropad ){
       MRI_IMAGE *qim ;
       qim = mri_zeropad_3D( pad_xm,pad_xp , pad_ym,pad_yp ,
                                             pad_zm,pad_zp , wbim ) ;
       mri_free(wbim) ; wbim = qim ;
     }
     if( wbim->nx != nx || wbim->ny != ny || wbim->nz != nz )
       ERROR_exit("-weight image doesn't match -base image grid") ;

   }

   /*--- scale weight volume so max value is 1 (and is all non-negative) ---*/

   { float fac , *wt ; int ii ;
     fac = (float)mri_max(wbim) ;
     if( fac <= 0.0f ) ERROR_exit("weight volume is not positive?!") ;
     fac = 1.0f / fac ; wt = MRI_FLOAT_PTR(wbim) ;
     for( ii=0 ; ii < wbim->nvox ; ii++ )
       wt[ii] = ( wt[ii] <= 0.0f ) ? 0.0f : fac * wt[ii] ;
   }

   /*----- blur base here if so ordered (source is blurred in warpomatic) ----*/

   if( Hblur_b >= 0.5f && !do_plusminus ){
     MRI_IMAGE *qim ;
     if( Hverb > 1 ) ININFO_message("   blurring base image %.3g voxels FWHM",Hblur_b) ;
     qim = mri_float_blur3D( FWHM_TO_SIGMA(Hblur_b) , bim ) ;
     mri_free(bim) ; bim = qim ;
   } else if( Hblur_b <= -1.0f && !do_plusminus ){
     MRI_IMAGE *qim ;
     if( Hverb > 1 ) ININFO_message("   median-izing base image %.3g voxels",-Hblur_b) ;
     qim = mri_medianfilter( bim , -Hblur_b , NULL , 0 ) ;
     mri_free(bim) ; bim = qim ;
   }

   /*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
   /*------------------------- do some actual work! --------------------------*/

   if( Hverb )
     INFO_message("+++++++++++ Begin warp optimization:  base=%s  source=%s" ,
                  DSET_HEADNAME(bset) , DSET_HEADNAME(sset) ) ;

   if( do_plusminus ){   /*--- special case of plusminus warp ----------------*/

#ifndef ALLOW_PLUSMINUS
     ERROR_exit("-plusminus: This message should never appear!") ;
#else
     sbww = IW3D_warp_s2bim_plusminus( bim,wbim,sim, MRI_WSINC5, meth, flags ) ;
     oiw  = sbww[0] ;  /* plus warp and image */
     qiw  = sbww[1] ;  /* minus warp and image */
#endif

   } else {              /*--- the standard case -----------------------------*/

     qiw = NULL ;
     if( duplo )
       oiw = IW3D_warp_s2bim_duplo( bim,wbim,sim, MRI_WSINC5, meth, flags ) ;
     else
       oiw = IW3D_warp_s2bim( bim,wbim,sim, MRI_WSINC5, meth, flags ) ;

   }

   /*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
   /**----------------------- check for errorosities ------------------------**/

   if( oiw == NULL ) ERROR_exit("s2bim fails") ;

   INFO_message("========== total number of parameters 'optimized' = %d",Hnpar_sum) ;

   mri_free(wbim) ; wbim = NULL ;  /* not needed after here [17 Oct 2013] */

   /*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
   /*--------- Post-processing to get the datasets to write to disk ----------*/

   /*---------- un-zeropad the output stuff, if needed and desired -----------*/

   if( zeropad ){
     MRI_IMAGE *qim ; IndexWarp3D *QQ ;

     /* crop the output image (maybe) */
     if( oiw->im->nx > nxold || oiw->im->ny > nyold || oiw->im->nz > nzold ){
       if( Hverb > 1 ) INFO_message("un-zero-padding output volume back to original base grid") ;
       qim = mri_zeropad_3D( -pad_xm,-pad_xp, -pad_ym,-pad_yp, -pad_zm,-pad_zp, oiw->im ) ;
       mri_free(oiw->im) ; oiw->im = qim ;
     }

     /* crop the output warp (maybe) */
     if( !zeropad_warp ){
       if( Hverb > 1 ) ININFO_message("un-zero-padding warp back to original base grid") ;
       QQ = IW3D_extend( oiw->warp, -pad_xm,-pad_xp, -pad_ym,-pad_yp, -pad_zm,-pad_zp , 0 ) ;
       IW3D_destroy(oiw->warp) ; oiw->warp = QQ ;
     }

     /* same stuff for the plusminus warp results as well */
     if( qiw != NULL ){
       if( qiw->im->nx > nxold || qiw->im->ny > nyold || qiw->im->nz > nzold ){
         qim = mri_zeropad_3D( -pad_xm,-pad_xp, -pad_ym,-pad_yp, -pad_zm,-pad_zp, qiw->im ) ;
         mri_free(qiw->im) ; qiw->im = qim ;
       }
       if( !zeropad_warp ){
         QQ = IW3D_extend( qiw->warp, -pad_xm,-pad_xp, -pad_ym,-pad_yp, -pad_zm,-pad_zp , 0 ) ;
         IW3D_destroy(qiw->warp) ; qiw->warp = QQ ;
       }
     }

   }  /*---------- end of patching up for zeropad ----------*/

   /*--- make the warps adopt a dataset to specify their extrinsic geometry --*/

   oim = oiw->im ; oww = oiw->warp ;
   if( !zeropad_warp )
     adset = bset ;      /* adset = adoption parent */
   else
     adset = THD_zeropad( bset ,
                          pad_xm,pad_xp , pad_ym,pad_yp , pad_zm,pad_zp ,
                          "BSET_zeropadded" , ZPAD_IJK | ZPAD_EMPTY ) ;

                      IW3D_adopt_dataset( oww       , adset ) ;
   if( qiw  != NULL ) IW3D_adopt_dataset( qiw->warp , adset ) ;

   /*-------------------------------------------------------------------------*/
   /*-- Special case of pre-3dAllineate: adjust output warp and image (oiw) --*/

   if( do_allin || do_resam ){

     /* adjust warp for 3dAllineate matrix that came earlier */
     /* (don't have to adjust plusminus warp since can't be run with -allin) */

     if( do_allin ){
       mat44 tmat,smat,qmat ; IndexWarp3D *tarp ;
STATUS("adjust for 3dAllineate matrix") ;
       qmat = allin_matrix ;                            /* convert matrix to */
       tmat = MAT44_MUL(qmat,oiw->warp->cmat) ;         /* index space from  */
       smat = MAT44_MUL(oiw->warp->imat,tmat) ;         /* coordinate space  */
       tarp = IW3D_compose_w1m2(oiw->warp,smat,MRI_WSINC5) ;  /* adjust warp */
       IW3D_destroy(oiw->warp) ; oww = oiw->warp = tarp ;
       IW3D_adopt_dataset(oww,adset) ;
     }

     /** directly warp from original source dataset to output image **/
     /** (then replace existing oiw->im with this re-warped image)  **/

     if( !nodset ){
       THD_3dim_dataset *wset, *iset ; MRI_IMAGE *iim ;
       wset = IW3D_to_dataset( oiw->warp , "ZharkTheGlorious" ) ;
       iset = THD_nwarp_dataset( wset , sstrue , bset , "WhoTheHellCares" ,
                                 MRI_WSINC5,MRI_WSINC5, 0.0f,1.0f, 1, NULL ) ;
       if( iset == NULL )  /* should be impossible */
         ERROR_exit("Can't warp from original dataset for some reason :-(") ;
       iim = THD_extract_float_brick(0,iset) ;
       mri_free(oiw->im) ; oim = oiw->im = iim ; DSET_delete(iset) ; DSET_delete(wset) ;

       if( noneg && mri_min(oiw->im) < 0.0f ){   /* clipping */
         float *oar = MRI_FLOAT_PTR(oiw->im) ; int ii ;
         for( ii=0 ; ii < oiw->im->nvox ; ii++ ){
           if( oar[ii] < 0.0f ) oar[ii] = 0.0 ;
         }
       }
     }

   } /*--------- end of patchup for input from 3dAllineate ---------*/

#if 0
   if( Hverb > 1 )
     INFO_message("output warp energy = %g",IW3D_load_energy(oww)) ;
#endif

   /*-------------------------------------------------------------------------*/
   /*------------ finally, output some results to pacify the user ------------*/

   if( !nodset ){                    /*----- output warped dataset -----*/
     char *qprefix = prefix ;
STATUS("output warped dataset") ;
     if( do_plusminus ){
       sprintf(appendage,"_%s",plusname) ;
       qprefix = modify_afni_prefix(prefix,NULL,appendage) ;
     }
     oset = EDIT_empty_copy(bset) ;
     tross_Copy_History( bset , oset ) ;
     tross_Make_History( "3dQwarp" , argc,argv , oset ) ;
     EDIT_dset_items( oset ,
                        ADN_prefix    , qprefix ,
                        ADN_nvals     , 1 ,
                        ADN_ntt       , 0 ,
                        ADN_datum_all , MRI_float ,
                      ADN_none ) ;
     EDIT_BRICK_FACTOR(oset,0,0.0) ;
     EDIT_substitute_brick( oset, 0, MRI_float, MRI_FLOAT_PTR(oim) ) ;
     DSET_write(oset) ;
     WROTE_DSET(oset) ;
     DSET_delete(oset) ;

     if( do_plusminus && qiw != NULL ){
       sprintf(appendage,"_%s",minusname) ;
       qprefix = modify_afni_prefix(prefix,NULL,appendage) ;
       oset = EDIT_empty_copy(bset) ;
       tross_Copy_History( bset , oset ) ;
       tross_Make_History( "3dQwarp" , argc,argv , oset ) ;
       EDIT_dset_items( oset ,
                          ADN_prefix    , qprefix ,
                          ADN_nvals     , 1 ,
                          ADN_ntt       , 0 ,
                          ADN_datum_all , MRI_float ,
                        ADN_none ) ;
       EDIT_BRICK_FACTOR(oset,0,0.0) ;
       EDIT_substitute_brick( oset, 0, MRI_float, MRI_FLOAT_PTR(qiw->im) ) ;
       DSET_write(oset) ; WROTE_DSET(oset) ; DSET_delete(oset) ;
     }
   } /* end of writing warped datasets */

#ifdef USE_SAVER
   if( qset != NULL && DSET_NVALS(qset) > 1 ){
     EDIT_dset_items( qset , ADN_ntt , DSET_NVALS(qset) , ADN_ttdel , 1.0f , ADN_none ) ;
     DSET_write(qset) ; WROTE_DSET(qset) ; DSET_delete(qset) ;
   }
#endif

   if( !nowarp ){                 /*----- output the warp itself -----*/
     char *qprefix ;
STATUS("output warp") ;
     if( do_plusminus){
       sprintf(appendage,"_%s_WARP",plusname) ;
       qprefix = modify_afni_prefix(prefix,NULL,appendage) ;
     } else {
       qprefix = modify_afni_prefix(prefix,NULL,"_WARP") ;
     }
     qset = IW3D_to_dataset( oww , qprefix ) ;
#if 0
INFO_message("warp dataset origin: %g %g %g",DSET_XORG(qset),DSET_YORG(qset),DSET_ZORG(qset)) ;
#endif
     tross_Copy_History( bset , qset ) ;
     tross_Make_History( "3dQwarp" , argc,argv , qset ) ;
     MCW_strncpy( qset->atlas_space , bset->atlas_space , THD_MAX_NAME ) ;
     DSET_write(qset) ; WROTE_DSET(qset) ; DSET_delete(qset) ;

     if( do_plusminus && qiw != NULL ){
       sprintf(appendage,"_%s_WARP",minusname) ;
       qprefix = modify_afni_prefix(prefix,NULL,appendage) ;
       qset = IW3D_to_dataset( qiw->warp , qprefix ) ;
       tross_Copy_History( bset , qset ) ;
       tross_Make_History( "3dQwarp" , argc,argv , qset ) ;
       MCW_strncpy( qset->atlas_space , bset->atlas_space , THD_MAX_NAME ) ;
       DSET_write(qset) ; WROTE_DSET(qset) ; DSET_delete(qset) ;
     }
   } /* end of output of warp dataset */

   if( !nowarpi && !do_plusminus ){      /*----- output the inverse warp -----*/
     if( Hverb ) fprintf(stderr,"++ Inverting warp ") ;
     owwi = IW3D_invert( oiw->warp , NULL , MRI_WSINC5 ) ;
     if( Hverb ) fprintf(stderr,"\n") ;
     IW3D_adopt_dataset( owwi , adset ) ;
     qset = IW3D_to_dataset( owwi, modify_afni_prefix(prefix,NULL,"_WARPINV")) ;
     tross_Copy_History( bset , qset ) ;
     tross_Make_History( "3dQwarp" , argc,argv , qset ) ;
     MCW_strncpy( qset->atlas_space , bset->atlas_space , THD_MAX_NAME ) ;
     DSET_write(qset) ; WROTE_DSET(qset) ; DSET_delete(qset) ;
     IW3D_destroy(owwi) ; owwi = NULL ;
   }

   /*------------- go back to watching Matlock reruns ------------------------*/

STATUS("watching Matlock reruns") ;

   cput = COX_cpu_time() ;
   if( cput > 0.05 )
     INFO_message("===== CPU time = %.1f sec  clock time =%s",
                  cput , nice_time_string(NI_clock_time()-ct) ) ;
   else
     INFO_message("===== clock time =%s" , nice_time_string(NI_clock_time()-ct) ) ;

   exit(0) ;
}
