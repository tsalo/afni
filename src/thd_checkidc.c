#include "mrilib.h"
#include "thd.h"

/*****************************************************************************
  This software is copyrighted and owned by the Medical College of Wisconsin.
  See the file README.Copyright for details.
******************************************************************************/

/*----------------------------------------------------------------
    Check to see if there are any duplicate ID codes in the
    datasets stored herein.
------------------------------------------------------------------*/

void THD_check_idcodes( THD_sessionlist * ssl )
{
   int iss , idd,jdd , ivv , dsnum , nd ;
   THD_session * sess ;
   THD_3dim_dataset * dset , ** dsl ;

   /*-- sanity check --*/

   if( ! ISVALID_SESSIONLIST(ssl) || ssl->num_sess <= 0 ) return ;

   /*-- count number of datasets --*/

   for( dsnum=iss=0 ; iss < ssl->num_sess ; iss++ ){
      sess = ssl->ssar[iss] ;
      for( idd=0 ; idd < sess->num_anat ; idd++ ){
         for( ivv=FIRST_VIEW_TYPE ; ivv <= LAST_VIEW_TYPE ; ivv++ ){
            dset = sess->anat[idd][ivv] ;
            if( ISVALID_DSET(dset) ) dsnum++ ;
         }
      }
      for( idd=0 ; idd < sess->num_func ; idd++ ){
         for( ivv=FIRST_VIEW_TYPE ; ivv <= LAST_VIEW_TYPE ; ivv++ ){
            dset = sess->func[idd][ivv] ;
            if( ISVALID_DSET(dset) ) dsnum++ ;
         }
      }
   }

   /*-- make list of datasets --*/

   dsl = (THD_3dim_dataset **) malloc( sizeof(THD_3dim_dataset *) * dsnum ) ;

   for( nd=iss=0 ; iss < ssl->num_sess ; iss++ ){
      sess = ssl->ssar[iss] ;
      for( idd=0 ; idd < sess->num_anat ; idd++ ){
         for( ivv=FIRST_VIEW_TYPE ; ivv <= LAST_VIEW_TYPE ; ivv++ ){
            dset = sess->anat[idd][ivv] ;
            if( ISVALID_DSET(dset) ) dsl[nd++] = dset ;
         }
      }
      for( idd=0 ; idd < sess->num_func ; idd++ ){
         for( ivv=FIRST_VIEW_TYPE ; ivv <= LAST_VIEW_TYPE ; ivv++ ){
            dset = sess->func[idd][ivv] ;
            if( ISVALID_DSET(dset) ) dsl[nd++] = dset ;
         }
      }
   }

   /*-- check list for duplicates --*/

   for( iss=idd=0 ; idd < dsnum-1 ; idd++ ){
      nd = 0 ;
      for( jdd=idd+1 ; jdd < dsnum ; jdd++ ){
         if( EQUIV_IDCODES( dsl[idd]->idcode , dsl[jdd]->idcode ) ){
            fprintf(stderr,
                    "\n*** WARNING: Identical ID codes in %s and %s",
                    DSET_HEADNAME(dsl[idd]) , DSET_HEADNAME(dsl[jdd]) ) ;
            iss++ ;
         }
      }
   }

   if( iss > 0 ) fprintf(stderr,"\n") ;

   free(dsl) ; return ;
}
