/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
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
#include "condor_attributes.h"
#include "condor_debug.h"
#include "condor_string.h"	// for strnewp and friends
#include "condor_daemon_core.h"
#include "spooled_job_files.h"
#include "daemon.h"
#include "dc_schedd.h"
#include "job_lease.h"
#include "nullfile.h"

#include "gridmanager.h"
#include "condorjob.h"
#include "condor_config.h"
#include "file_transfer.h"


// GridManager job states
#define GM_INIT					0
#define GM_REGISTER				1
#define GM_STDIO_UPDATE			2
#define GM_UNSUBMITTED			3
#define GM_SUBMIT				4
#define GM_SUBMIT_SAVE			5
#define GM_STAGE_IN				6
#define GM_SUBMITTED			7
#define GM_DONE_SAVE			8
#define GM_DONE_COMMIT			9
#define GM_CANCEL				10
#define GM_FAILED				11
#define GM_DELETE				12
#define GM_CLEAR_REQUEST		13
#define GM_HOLD					14
#define GM_PROXY_EXPIRED		15
#define GM_REFRESH_PROXY		16
#define GM_START				17
#define GM_HOLD_REMOTE_JOB		18
#define GM_RELEASE_REMOTE_JOB	19
#define GM_POLL_ACTIVE			20
#define GM_STAGE_OUT			21
#define GM_RECOVER_POLL			22

static const char *GMStateNames[] = {
	"GM_INIT",
	"GM_REGISTER",
	"GM_STDIO_UPDATE",
	"GM_UNSUBMITTED",
	"GM_SUBMIT",
	"GM_SUBMIT_SAVE",
	"GM_STAGE_IN",
	"GM_SUBMITTED",
	"GM_DONE_SAVE",
	"GM_DONE_COMMIT",
	"GM_CANCEL",
	"GM_FAILED",
	"GM_DELETE",
	"GM_CLEAR_REQUEST",
	"GM_HOLD",
	"GM_PROXY_EXPIRED",
	"GM_REFRESH_PROXY",
	"GM_START",
	"GM_HOLD_REMOTE_JOB",
	"GM_RELEASE_REMOTE_JOB",
	"GM_POLL_ACTIVE",
	"GM_STAGE_OUT",
	"GM_RECOVER_POLL"
};

#define JOB_STATE_UNKNOWN				-1
#define JOB_STATE_UNSUBMITTED			0

// TODO: Let the maximum submit attempts be set in the job ad or, better yet,
// evalute PeriodicHold expression in job ad.
#define MAX_SUBMIT_ATTEMPTS	1

#define LOG_GLOBUS_ERROR(func,error) \
    dprintf(D_ALWAYS, \
		"(%d.%d) gmState %s, remoteState %d: %s returned error %d\n", \
        procID.cluster,procID.proc,GMStateNames[gmState],remoteState, \
        func,error)



void CondorJobInit()
{
}

void CondorJobReconfig()
{
	int tmp_int;

	tmp_int = param_integer( "CONDOR_JOB_POLL_INTERVAL", 5 * 60 );
	CondorResource::setPollInterval( tmp_int );

	tmp_int = param_integer( "GRIDMANAGER_GAHP_CALL_TIMEOUT", 8 * 60 * 60 );
	tmp_int = param_integer( "GRIDMANAGER_GAHP_CALL_TIMEOUT_CONDOR", tmp_int );
	CondorJob::setGahpCallTimeout( tmp_int );

	tmp_int = param_integer("GRIDMANAGER_CONNECT_FAILURE_RETRY_COUNT",3);
	CondorJob::setConnectFailureRetry( tmp_int );

	// Tell all the resource objects to deal with their new config values
	CondorResource *next_resource;

	CondorResource::ResourcesByName.startIterations();

	while ( CondorResource::ResourcesByName.iterate( next_resource ) != 0 ) {
		next_resource->Reconfig();
	}

}

bool CondorJobAdMatch( const ClassAd *job_ad ) {
	int universe;
	std::string resource;
	if ( job_ad->LookupInteger( ATTR_JOB_UNIVERSE, universe ) &&
		 universe == CONDOR_UNIVERSE_GRID &&
		 job_ad->LookupString( ATTR_GRID_RESOURCE, resource ) &&
		 strncasecmp( resource.c_str(), "condor ", 7 ) == 0 ) {

		return true;
	}
	return false;
}

BaseJob *CondorJobCreate( ClassAd *jobad )
{
	return (BaseJob *)new CondorJob( jobad );
}


int CondorJob::submitInterval = 300;			// default value
int CondorJob::removeInterval = 30;			// default value
int CondorJob::gahpCallTimeout = 8*60*60;		// default value
int CondorJob::maxConnectFailures = 3;			// default value

CondorJob::CondorJob( ClassAd *classad )
	: BaseJob( classad )
{
	char buff[4096];
	ArgList args;
	std::string error_string = "";
	char *gahp_path;
	bool job_already_submitted = false;

	remoteJobId.cluster = 0;
	gahpAd = NULL;
	gmState = GM_INIT;
	remoteState = JOB_STATE_UNKNOWN;
	enteredCurrentGmState = time(NULL);
	enteredCurrentRemoteState = time(NULL);
	lastSubmitAttempt = 0;
	lastRemoveAttempt = 0;
	numSubmitAttempts = 0;
	submitFailureCode = 0;
	remoteScheddName = NULL;
	remotePoolName = NULL;
	submitterId = NULL;
	connectFailureCount = 0;
	jobProxy = NULL;
	lastProxyExpireTime = 0;
	delegatedProxyExpireTime = 0;
	delegatedProxyRenewTime = 0;
	lastProxyRefreshAttempt = 0;
	myResource = NULL;
	newRemoteStatusAd = NULL;
	newRemoteStatusServerTime = 0;
	doActivePoll = false;
	int int_value;

	lastRemoteStatusServerTime = 0;

	gahp = NULL;

	// In GM_HOLD, we assume HoldReason to be set only if we set it, so make
	// sure it's unset when we start.
	if ( jobAd->LookupString( ATTR_HOLD_REASON, NULL, 0 ) != 0 ) {
		jobAd->AssignExpr( ATTR_HOLD_REASON, "Undefined" );
	}

	jobProxy = AcquireProxy( jobAd, error_string,
							 (TimerHandlercpp)&BaseJob::SetEvaluateState, this );
	if ( jobProxy == NULL && error_string != "" ) {
		goto error_exit;
	}

	int_value = 0;
	if ( jobAd->LookupInteger( ATTR_DELEGATED_PROXY_EXPIRATION, int_value ) ) {
		delegatedProxyExpireTime = (time_t)int_value;
		delegatedProxyRenewTime = GetDelegatedProxyRenewalTime(delegatedProxyExpireTime);
	}

	buff[0] = '\0';
	jobAd->LookupString( ATTR_GRID_RESOURCE, buff );
	if ( buff[0] != '\0' ) {
		const char *token;

		Tokenize( buff );

		token = GetNextToken( " ", false );
		if ( !token || strcasecmp( token, "condor" ) ) {
			sprintf( error_string, "%s not of type condor",
								  ATTR_GRID_RESOURCE );
			goto error_exit;
		}

		token = GetNextToken( " ", false );
		if ( token && *token ) {
			remoteScheddName = strdup( token );
		} else {
			sprintf( error_string, "%s missing schedd name",
								  ATTR_GRID_RESOURCE );
			goto error_exit;
		}

		token = GetNextToken( " ", false );
		if ( token && *token ) {
			remotePoolName = strdup( token );
		} else {
			sprintf( error_string, "%s missing pool name",
								  ATTR_GRID_RESOURCE );
			goto error_exit;
		}

	} else {
		sprintf( error_string, "%s is not set in the job ad",
							  ATTR_GRID_RESOURCE );
		goto error_exit;
	}

	buff[0] = '\0';
	jobAd->LookupString( ATTR_GRID_JOB_ID, buff );
	if ( buff[0] != '\0' ) {
		SetRemoteJobId( strrchr( buff, ' ' )+1 );
		job_already_submitted = true;
	} else {
		remoteState = JOB_STATE_UNSUBMITTED;
	}

	buff[0] = '\0';
	jobAd->LookupString( ATTR_GLOBAL_JOB_ID, buff );
	if ( buff[0] != '\0' ) {
		char *ptr = strchr( buff, '#' );
		if ( ptr != NULL ) {
			*ptr = '\0';
		}
		submitterId = strdup( buff );
	} else {
		sprintf( error_string, "%s is not set in the job ad",
							  ATTR_GLOBAL_JOB_ID );
		goto error_exit;
	}

	myResource = CondorResource::FindOrCreateResource( remoteScheddName,
													   remotePoolName,
													   jobProxy );
	myResource->RegisterJob( this, submitterId );
	if ( job_already_submitted ) {
		myResource->AlreadySubmitted( this );
	}

	gahp_path = param("CONDOR_GAHP");
	if ( gahp_path == NULL ) {
		error_string = "CONDOR_GAHP not defined";
		goto error_exit;
	}
		// TODO remove remoteScheddName from the gahp server key if/when
		//   a gahp server can handle multiple schedds
	sprintf( buff, "CONDOR/%s/%s/%s", remotePoolName ? remotePoolName : "NULL",
			 remoteScheddName,
			 jobProxy != NULL ? jobProxy->subject->fqan : "NULL" );
	args.AppendArg("-f");
	args.AppendArg("-s");
	args.AppendArg(remoteScheddName);
	if ( remotePoolName ) {
		args.AppendArg("-P");
		args.AppendArg(remotePoolName);
	}
	gahp = new GahpClient( buff, gahp_path, &args );
	free( gahp_path );

	gahp->setNotificationTimerId( evaluateStateTid );
	gahp->setMode( GahpClient::normal );
	gahp->setTimeout( gahpCallTimeout );

	return;

 error_exit:
		// We must ensure that the code-path from GM_HOLD doesn't depend
		// on any initialization that's been skipped.
	gmState = GM_HOLD;
	if ( !error_string.empty() ) {
		jobAd->Assign( ATTR_HOLD_REASON, error_string.c_str() );
	}
	return;
}

CondorJob::~CondorJob()
{
	if ( jobProxy != NULL ) {
		ReleaseProxy( jobProxy, (TimerHandlercpp)&BaseJob::SetEvaluateState, this );
	}
	if ( submitterId != NULL ) {
		free( submitterId );
	}
	if ( newRemoteStatusAd != NULL ) {
		delete newRemoteStatusAd;
	}
	if ( myResource ) {
		myResource->UnregisterJob( this );
	}
	if ( remoteScheddName ) {
		free( remoteScheddName );
	}
	if ( remotePoolName ) {
		free( remotePoolName );
	}
	if ( gahpAd ) {
		delete gahpAd;
	}
	if ( gahp != NULL ) {
		delete gahp;
	}
}

void CondorJob::Reconfig()
{
	BaseJob::Reconfig();
	gahp->setTimeout( gahpCallTimeout );
}

void CondorJob::JobLeaseSentExpired()
{
dprintf(D_FULLDEBUG,"(%d.%d) CondorJob::JobLeaseSentExpired()\n",procID.cluster,procID.proc);
	BaseJob::JobLeaseSentExpired();
	SetRemoteJobId( NULL );
		// We always want to go through GM_INIT. With the remote job id set
		// to NULL, we'll go to GM_CLEAR_REQUEST afterwards.
	if ( gmState != GM_INIT ) {
		gmState = GM_CLEAR_REQUEST;
	}
}

void CondorJob::doEvaluateState()
{
	bool connect_failure = false;
	int old_gm_state;
	int old_remote_state;
	bool reevaluate_state = true;
	time_t now = time(NULL);

	bool attr_exists;
	bool attr_dirty;
	int rc;

	daemonCore->Reset_Timer( evaluateStateTid, TIMER_NEVER );

    dprintf(D_ALWAYS,
			"(%d.%d) doEvaluateState called: gmState %s, remoteState %d\n",
			procID.cluster,procID.proc,GMStateNames[gmState],remoteState);

	if ( gahp ) {
		if ( !resourceStateKnown || resourcePingPending || resourceDown ) {
			gahp->setMode( GahpClient::results_only );
		} else {
			gahp->setMode( GahpClient::normal );
		}
	}

	do {
		reevaluate_state = false;
		old_gm_state = gmState;
		old_remote_state = remoteState;

		switch ( gmState ) {
		case GM_INIT: {
			// This is the state all jobs start in when the GlobusJob object
			// is first created. Here, we do things that we didn't want to
			// do in the constructor because they could block (the
			// constructor is called while we're connected to the schedd).
			if ( gahp->Startup() == false ) {
				dprintf( D_ALWAYS, "(%d.%d) Error starting GAHP\n",
						 procID.cluster, procID.proc );

				jobAd->Assign( ATTR_HOLD_REASON, "Failed to start GAHP" );
				gmState = GM_HOLD;
				break;
			}
			if ( jobProxy && gahp->Initialize( jobProxy ) == false ) {
				dprintf( D_ALWAYS, "(%d.%d) Error initializing GAHP\n",
						 procID.cluster, procID.proc );

				jobAd->Assign( ATTR_HOLD_REASON,
							   "Failed to initialize GAHP" );
				gmState = GM_HOLD;
				break;
			}

			gmState = GM_START;
			} break;
		case GM_START: {
			// This state is the real start of the state machine, after
			// one-time initialization has been taken care of.
			// If we think there's a running jobmanager
			// out there, we try to register for callbacks (in GM_REGISTER).
			// The one way jobs can end up back in this state is if we
			// attempt a restart of a jobmanager only to be told that the
			// old jobmanager process is still alive.
			errorString = "";
			if ( remoteJobId.cluster == 0 ) {
				if( condorState == COMPLETED ) {
					gmState = GM_DELETE;
				} else {
					gmState = GM_CLEAR_REQUEST;
				}
			} else if ( condorState == COMPLETED ) {
				gmState = GM_DONE_COMMIT;
			} else if ( condorState == REMOVED ) {
				gmState = GM_CANCEL;
			} else {
				gmState = GM_RECOVER_POLL;
			}
			} break;
		case GM_RECOVER_POLL: {
			int num_ads = 0;
			int tmp_int = 0;
			ClassAd **status_ads = NULL;
			std::string constraint;
			sprintf( constraint, "%s==%d&&%s==%d", ATTR_CLUSTER_ID,
								remoteJobId.cluster, ATTR_PROC_ID,
								remoteJobId.proc );
			rc = gahp->condor_job_status_constrained( remoteScheddName,
													  constraint.c_str(),
													  &num_ads, &status_ads );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			if ( rc != GLOBUS_SUCCESS ) {
				// unhandled error
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_status_constrained() failed: %s\n",
						 procID.cluster, procID.proc, gahp->getErrorString() );
				if ( !resourcePingComplete /* && connect failure */ ) {
					connect_failure = true;
					break;
				}
				errorString = gahp->getErrorString();
				gmState = GM_HOLD;
				break;
			}
			if ( num_ads == 0 ) {
					// The job isn't there. Assume it timed out before we
					// could stage in the data files.
				SetRemoteJobId( NULL );
				gmState = GM_CLEAR_REQUEST;
			} else if ( num_ads != 1 ) {
					// Didn't get the number of ads we expected. Abort!
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_status_constrained() returned %d ads\n",
						 procID.cluster, procID.proc, num_ads );
				errorString = "Remote schedd returned multiple ads";
				gmState = GM_CANCEL;
			} else if ( status_ads[0]->LookupInteger( ATTR_STAGE_IN_FINISH,
													  tmp_int ) == 0 ||
						tmp_int <= 0 ) {
					// We haven't finished staging in files yet
				gmState = GM_STAGE_IN;
			} else {

					// Copy any attributes we care about from the remote
					// ad to our local one before doing anything else
				ProcessRemoteAd( status_ads[0] );
				int server_time;
				if ( status_ads[0]->LookupInteger( ATTR_SERVER_TIME,
												   server_time ) == 0 ) {
					dprintf( D_ALWAYS, "(%d.%d) Ad from remote schedd has "
							 "no %s, faking with current local time\n",
							 procID.cluster, procID.proc, ATTR_SERVER_TIME );
					server_time = time(NULL);
				}
				lastRemoteStatusServerTime = server_time;

				if ( remoteState == COMPLETED &&
					 status_ads[0]->LookupInteger( ATTR_STAGE_OUT_FINISH,
												   tmp_int ) != 0 &&
					 tmp_int > 0 ) {

						// We already staged out all the files
					gmState = GM_DONE_SAVE;
				} else {
						// All other cases can be handled by GM_SUBMITTED
					gmState = GM_SUBMITTED;
				}
			}
			for ( int i = 0; i < num_ads; i++ ) {
				delete status_ads[i];
			}
			free( status_ads );
			} break;
		case GM_UNSUBMITTED: {
			// There are no outstanding remote submissions for this job (if
			// there is one, we've given up on it).
			if ( condorState == REMOVED ) {
				gmState = GM_DELETE;
			} else if ( condorState == HELD ) {
				gmState = GM_DELETE;
				break;
			} else if ( condorState == COMPLETED ) {
				gmState = GM_DELETE;
			} else {
				gmState = GM_SUBMIT;
			}
			} break;
		case GM_SUBMIT: {
			// Start a new remote submission for this job.
			if ( condorState == REMOVED || condorState == HELD ) {
				myResource->CancelSubmit(this);
				gmState = GM_UNSUBMITTED;
				break;
			}
			if ( numSubmitAttempts >= MAX_SUBMIT_ATTEMPTS ) {
				errorString = "Repeated submit attempts (GAHP reports:";
				errorString += gahp->getErrorString();
				errorString += ")";
				gmState = GM_HOLD;
				break;
			}
			// After a submit, wait at least submitInterval before trying
			// another one.
			if ( now >= lastSubmitAttempt + submitInterval ) {
				// Once RequestSubmit() is called at least once, you must
				// call CancelSubmit() when there's no job left on the
				// remote host and you don't plan to submit one.
				if ( myResource->RequestSubmit(this) == false ) {
					break;
				}
				char *job_id_string = NULL;
				if ( gahpAd == NULL ) {
					int new_expiration;
					if ( CalculateJobLease( jobAd, new_expiration ) ) {
							// This will set the job lease sent attrs,
							// which get referenced in buildSubmitAd()
						UpdateJobLeaseSent( new_expiration );
					}
					gahpAd = buildSubmitAd();
				}
				if ( gahpAd == NULL ) {
					errorString = "Internal Error: Failed to build submit ad.";
					gmState = GM_HOLD;
					break;
				}
				rc = gahp->condor_job_submit( remoteScheddName,
											  gahpAd,
											  &job_id_string );
				if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
					 rc == GAHPCLIENT_COMMAND_PENDING ) {
					break;
				}
				lastSubmitAttempt = time(NULL);
				numSubmitAttempts++;
				if ( rc == GLOBUS_SUCCESS ) {
					SetRemoteJobId( job_id_string );
					WriteGridSubmitEventToUserLog( jobAd );
					gmState = GM_SUBMIT_SAVE;
				} else {
					dprintf( D_ALWAYS,
							 "(%d.%d) condor_job_submit() failed: %s\n",
							 procID.cluster, procID.proc,
							 gahp->getErrorString() );
					int jcluster = -1;
					int jproc = -1;
					if(job_id_string) {
							// job_id_string is null in many failure cases.
						sscanf( job_id_string, "%d.%d", &jcluster, &jproc );
					}
					// if the job failed to submit, the cluster number
					// will hold the error code for the call to 
					// NewCluster(), and the proc number will hold
					// error code for the call to NewProc().
					// now check if either call failed w/ -2, which
					// signifies MAX_JOBS_SUBMITTED was exceeded.
					if ( jcluster==-2 || jproc==-2 ) {
						// MAX_JOBS_SUBMITTED error.
						// For now, we will always put this job back
						// to idle and tell the schedd to find us
						// another match.
						// TODO: this hard-coded logic should be
						// replaced w/ a WANT_REMATCH expression, like
						// is currently done in the Globus gridtype.
						dprintf(D_ALWAYS,"(%d.%d) Requesting schedd to "
							"rematch job because of MAX_JOBS_SUBMITTED\n",
							procID.cluster, procID.proc);
						// Set ad attributes so the schedd finds a new match.
						int dummy;
						if ( jobAd->LookupBool( ATTR_JOB_MATCHED, dummy ) != 0 ) {
							jobAd->Assign( ATTR_JOB_MATCHED, false );
							jobAd->Assign( ATTR_CURRENT_HOSTS, 0 );
						}
						// We are rematching,  so forget about this job 
						// cuz we wanna pull a fresh new job ad, with 
						// a fresh new match, from the all-singing schedd.
						gmState = GM_DELETE;
					} else {
						// unhandled error
						if ( !resourcePingComplete /* && connect failure */ ) {
							numSubmitAttempts--;
							connect_failure = true;
							break;
						}
						gmState = GM_UNSUBMITTED;
						reevaluate_state = true;
					}
				}
				if ( job_id_string != NULL ) {
					free( job_id_string );
				}
			} else if ( condorState == REMOVED || condorState == HELD ) {
				gmState = GM_UNSUBMITTED;
			} else {
				unsigned int delay = 0;
				if ( (lastSubmitAttempt + submitInterval) > now ) {
					delay = (lastSubmitAttempt + submitInterval) - now;
				}				
				daemonCore->Reset_Timer( evaluateStateTid, delay );
			}
			} break;
		case GM_SUBMIT_SAVE: {
			// Save the job id for a new remote submission.
			if ( condorState == REMOVED || condorState == HELD ) {
				gmState = GM_CANCEL;
			} else {
				jobAd->GetDirtyFlag( ATTR_GRID_JOB_ID, &attr_exists, &attr_dirty );
				if ( attr_exists && attr_dirty ) {
					requestScheddUpdate( this, true );
					break;
				}
				gmState = GM_STAGE_IN;
			}
			} break;
		case GM_STAGE_IN: {
			// Now stage files to the remote schedd
			if ( condorState == REMOVED ) {
				gmState = GM_CANCEL;
				break;
			}
			if ( gahpAd == NULL ) {
				gahpAd = buildStageInAd();
			}
			if ( gahpAd == NULL ) {
				errorString = "Internal Error: Failed to build stage in ad.";
				gmState = GM_HOLD;
				break;
			}
			rc = gahp->condor_job_stage_in( remoteScheddName, gahpAd );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			if ( rc == 0 ) {
				// We go to GM_POLL_ACTIVE here because if we get a status
				// ad from our CondorResource that's from before the stage
				// in completed, we'll get confused by the jobStatus of
				// HELD. By doing an active probe, we'll automatically
				// ignore any update ads from before the probe.
				if(jobProxy) {
					lastProxyExpireTime = jobProxy->expiration_time;
					delegatedProxyExpireTime = GetDesiredDelegatedJobCredentialExpiration(jobAd);
					delegatedProxyRenewTime = GetDelegatedProxyRenewalTime(delegatedProxyExpireTime);
					time_t actual_expiration = delegatedProxyExpireTime;
					if( actual_expiration == 0 || actual_expiration > jobProxy->expiration_time ) {
						actual_expiration = jobProxy->expiration_time;
					}
					jobAd->Assign( ATTR_DELEGATED_PROXY_EXPIRATION,
								   (int)actual_expiration );
				}
				gmState = GM_POLL_ACTIVE;
			} else {
				// unhandled error
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_stage_in() failed: %s\n",
						 procID.cluster, procID.proc, gahp->getErrorString() );
				if ( !resourcePingComplete /* && connect failure */ ) {
					connect_failure = true;
					break;
				}
				errorString = gahp->getErrorString();
				gmState = GM_HOLD;
			}
			} break;
		case GM_SUBMITTED: {
			// The job has been submitted. Wait for completion or failure,
			// and poll the remote schedd occassionally to let it know
			// we're still alive.
			if ( condorState == REMOVED ) {
				gmState = GM_CANCEL;
			} else if ( newRemoteStatusAd != NULL ) {
				if ( newRemoteStatusServerTime <= lastRemoteStatusServerTime ) {
					dprintf( D_FULLDEBUG, "(%d.%d) Discarding old job status ad from collective poll\n",
							 procID.cluster, procID.proc );
				} else {
					ProcessRemoteAd( newRemoteStatusAd );
					lastRemoteStatusServerTime = newRemoteStatusServerTime;
				}
				delete newRemoteStatusAd;
				newRemoteStatusAd = NULL;
				reevaluate_state = true;
			} else if ( doActivePoll ) {
				doActivePoll = false;
				gmState = GM_POLL_ACTIVE;
			} else if ( remoteState == COMPLETED ) {
				gmState = GM_STAGE_OUT;
			} else if ( condorState == HELD ) {
				if ( remoteState == HELD ) {
					// The job is on hold both locally and remotely. We're
					// done, delete this job object from the gridmanager.
					gmState = GM_DELETE;
				} else {
					gmState = GM_HOLD_REMOTE_JOB;
				}
			} else if ( remoteState == HELD ) {
				// The job is on hold remotely but not locally. This means
				// the remote job needs to be released.
				gmState = GM_RELEASE_REMOTE_JOB;
			} else if ( jobProxy && lastProxyExpireTime < jobProxy->expiration_time ||
						jobProxy && delegatedProxyRenewTime < now ) {
				int interval = param_integer( "GRIDMANAGER_PROXY_REFRESH_INTERVAL", 10*60 );
				if ( now >= lastProxyRefreshAttempt + interval ) {
					gmState = GM_REFRESH_PROXY;
				} else {
					dprintf( D_ALWAYS, "(%d.%d) Delaying refresh of proxy\n",
							 procID.cluster, procID.proc );
					unsigned int delay = 0;
					delay = (lastProxyRefreshAttempt + interval) - now;
					daemonCore->Reset_Timer( evaluateStateTid, delay );
				}
			}
			} break;
		case GM_REFRESH_PROXY: {
			if ( condorState == REMOVED || condorState == HELD ) {
				gmState = GM_SUBMITTED;
			} else {
				rc = gahp->condor_job_refresh_proxy( remoteScheddName,
													 remoteJobId,
													 jobProxy->proxy_filename );
				if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
					 rc == GAHPCLIENT_COMMAND_PENDING ) {
					break;
				}
				if ( rc != GLOBUS_SUCCESS ) {
					// unhandled error
					dprintf( D_ALWAYS,
							 "(%d.%d) condor_job_refresh_proxy() failed: %s\n",
							 procID.cluster, procID.proc, gahp->getErrorString() );
					if ( !resourcePingComplete /* && connect failure */ ) {
						connect_failure = true;
						break;
					}
					errorString = gahp->getErrorString();

					if ( ( lastProxyExpireTime != 0 &&
						   lastProxyExpireTime < now + 60 ) ||
						 ( delegatedProxyExpireTime != 0 &&
						   delegatedProxyExpireTime < now + 60 ) ||
						 ( lastProxyExpireTime == 0 &&
						   jobProxy->near_expired ) ) {

						dprintf( D_ALWAYS,
								 "(%d.%d) Proxy almosted expired, cancelling job\n",
								 procID.cluster, procID.proc );
						gmState = GM_CANCEL;
						break;
					}
				} else {
					lastProxyExpireTime = jobProxy->expiration_time;
					delegatedProxyExpireTime = GetDesiredDelegatedJobCredentialExpiration(jobAd);
					delegatedProxyRenewTime = GetDelegatedProxyRenewalTime(delegatedProxyExpireTime);
					time_t actual_expiration = delegatedProxyExpireTime;
					if( actual_expiration == 0 || actual_expiration > jobProxy->expiration_time ) {
						actual_expiration = jobProxy->expiration_time;
					}
					jobAd->Assign( ATTR_DELEGATED_PROXY_EXPIRATION,
								   (int)actual_expiration );
				}
				lastProxyRefreshAttempt = time(NULL);
				gmState = GM_SUBMITTED;
			}
		} break;
		case GM_HOLD_REMOTE_JOB: {
			rc = gahp->condor_job_hold( remoteScheddName, remoteJobId,
										"by gridmanager" );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			if ( rc != GLOBUS_SUCCESS ) {
				const char *err = gahp->getErrorString();
				if ( strcmp( err, "Already done" ) ) {
					dprintf( D_FULLDEBUG,
							 "(%d.%d) Remote job is already held\n",
							 procID.cluster, procID.proc );
				} else {
					// unhandled error
					dprintf( D_ALWAYS,
							 "(%d.%d) condor_job_hold() failed: %s\n",
							 procID.cluster, procID.proc, err );
					if ( !resourcePingComplete /* && connect failure */ ) {
						connect_failure = true;
						break;
					}
					errorString = err;
					gmState = GM_HOLD;
					break;
				}
			}
			gmState = GM_POLL_ACTIVE;
			} break;
		case GM_RELEASE_REMOTE_JOB: {
			rc = gahp->condor_job_release( remoteScheddName, remoteJobId,
										   "by gridmanager" );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			if ( rc != GLOBUS_SUCCESS ) {
				const char *err = gahp->getErrorString();
				if ( strcmp( err, "Already done" ) ) {
					dprintf( D_FULLDEBUG,
							 "(%d.%d) Remote job is already released\n",
							 procID.cluster, procID.proc );
				} else {
					// unhandled error
					dprintf( D_ALWAYS,
							 "(%d.%d) condor_job_release() failed: %s\n",
							 procID.cluster, procID.proc, err );
					if ( !resourcePingComplete /* && connect failure */ ) {
						connect_failure = true;
						break;
					}
					errorString = err;
					gmState = GM_HOLD;
					break;
				}
			}
			gmState = GM_POLL_ACTIVE;
			} break;
		case GM_POLL_ACTIVE: {
			int num_ads;
			ClassAd **status_ads = NULL;
			std::string constraint;
			sprintf( constraint, "%s==%d&&%s==%d", ATTR_CLUSTER_ID,
								remoteJobId.cluster, ATTR_PROC_ID,
								remoteJobId.proc );
			rc = gahp->condor_job_status_constrained( remoteScheddName,
													  constraint.c_str(),
													  &num_ads, &status_ads );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			if ( rc != GLOBUS_SUCCESS ) {
				// unhandled error
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_status_constrained() failed: %s\n",
						 procID.cluster, procID.proc, gahp->getErrorString() );
				if ( !resourcePingComplete /* && connect failure */ ) {
					connect_failure = true;
					break;
				}
				errorString = gahp->getErrorString();
				gmState = GM_HOLD;
				break;
			}
			if ( num_ads != 1 ) {
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_status_constrained() returned %d ads\n",
						 procID.cluster, procID.proc, num_ads );
				errorString = "Remote schedd returned multiple ads";
				for ( int i = 0; i < num_ads; i++ ) {
					delete status_ads[i];
				}
				free( status_ads );
				gmState = GM_CANCEL;
				break;
			}
				// If we just finished staging input files, it's possible
				// to see the job still on hold (it can take the schedd a
				// few seconds to release the job). In this case, simply
				// retry the poll until the job is no longer on hold to
				// spool input files. Not the best solution, but it should
				// work.
			std::string hold_reason = "";
			status_ads[0]->LookupString( ATTR_HOLD_REASON, hold_reason );
			if ( hold_reason == "Spooling input data files" ) {
				dprintf( D_FULLDEBUG, "(%d.%d) Job not yet released from stage-in hold, retrying poll\n",
						 procID.cluster, procID.proc );
				delete status_ads[0];
				free( status_ads );
				reevaluate_state = true;
				break;
			}
			ProcessRemoteAd( status_ads[0] );
			int server_time;
			if ( status_ads[0]->LookupInteger( ATTR_SERVER_TIME,
											   server_time ) == 0 ) {
				dprintf( D_ALWAYS, "(%d.%d) Ad from remote schedd has no %s, "
						 "faking with current local time\n",
						 procID.cluster, procID.proc, ATTR_SERVER_TIME );
				server_time = time(NULL);
			}
			lastRemoteStatusServerTime = server_time;
			delete status_ads[0];
			free( status_ads );
			gmState = GM_SUBMITTED;
			} break;
		case GM_STAGE_OUT: {
			// Now stage files back from the remote schedd
			if ( condorState == REMOVED ) {
				gmState = GM_CANCEL;
				break;
			}
			rc = gahp->condor_job_stage_out( remoteScheddName, remoteJobId );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			if ( rc == 0 ) {
				gmState = GM_DONE_SAVE;
			} else {
				// unhandled error
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_stage_out() failed: %s\n",
						 procID.cluster, procID.proc, gahp->getErrorString() );
				if ( !resourcePingComplete /* && connect failure */ ) {
					connect_failure = true;
					break;
				}
				errorString = gahp->getErrorString();
				gmState = GM_HOLD;
			}
			} break;
		case GM_DONE_SAVE: {
			// Report job completion to the schedd.
			JobTerminated();
			if ( condorState == COMPLETED ) {
				jobAd->GetDirtyFlag( ATTR_JOB_STATUS, &attr_exists, &attr_dirty );
				if ( attr_exists && attr_dirty ) {
					requestScheddUpdate( this, true );
					break;
				}
			}
			gmState = GM_DONE_COMMIT;
			} break;
		case GM_DONE_COMMIT: {
			// Tell the remote schedd it can remove the job from the queue.
			if ( gahpAd == NULL ) {
				gahpAd = new ClassAd;
				gahpAd->Assign( ATTR_JOB_LEAVE_IN_QUEUE, false );
			}
			rc = gahp->condor_job_update( remoteScheddName, remoteJobId,
										  gahpAd );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			if ( rc != GLOBUS_SUCCESS ) {
				// unhandled error
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_update() failed: %s\n",
						 procID.cluster, procID.proc, gahp->getErrorString() );
					// TODO: Once we have pending-completed and
					//   pending-removed states, don't just give up. We
					//   can put the job on hold.
					// Should we request a ping here?
				dprintf( D_ALWAYS,
						 "(%d.%d) Failed to clean up remote job state, leaving it there.\n",
						 procID.cluster, procID.proc );
			}
			if ( condorState == COMPLETED || condorState == REMOVED ) {
				gmState = GM_DELETE;
				SetRemoteJobId( NULL );
			} else {
				// Clear the contact string here because it may not get
				// cleared in GM_CLEAR_REQUEST (it might go to GM_HOLD first).
				SetRemoteJobId( NULL );
				myResource->CancelSubmit( this );
				requestScheddUpdate( this, false );
				gmState = GM_CLEAR_REQUEST;
			}
			} break;
		case GM_CANCEL: {
			// We need to cancel the job submission.

			// If a remove attempt fails here, we keep retrying, waiting
			// removeInterval seconds between attempts.
			if ( lastRemoveAttempt + removeInterval > now ) {
				daemonCore->Reset_Timer( evaluateStateTid,
								(lastRemoveAttempt + removeInterval) - now );
				break;
			}

			rc = gahp->condor_job_remove( remoteScheddName, remoteJobId,
										  "by gridmanager" );
			if ( rc == GAHPCLIENT_COMMAND_NOT_SUBMITTED ||
				 rc == GAHPCLIENT_COMMAND_PENDING ) {
				break;
			}
			lastRemoveAttempt = now;
				// If the job has already been marked for removal or
				// is no longer in the queue, treat it as a success.
				// TODO parsing error strings meant for human consumption
				//   is a poor way to distinguish specific types of
				//   errors. We should have something more formalized
				//   in the GAHP protocol.
			if ( rc != GLOBUS_SUCCESS &&
				 strcmp( gahp->getErrorString(), "Job not found" ) != 0 &&
				 strcmp( gahp->getErrorString(), "Already done" ) != 0 ) {

					// unhandled error
					// Keep retrying. Once we have leases, we can give up
					// once the lease expires.
					// Should we request a ping here?
				dprintf( D_ALWAYS,
						 "(%d.%d) condor_job_remove() failed (will retry): %s\n",
						 procID.cluster, procID.proc, gahp->getErrorString() );
				daemonCore->Reset_Timer( evaluateStateTid,
										 now + lastRemoveAttempt );
				break;
			}
			SetRemoteJobId( NULL );

			if ( condorState == REMOVED ) {
				gmState = GM_DELETE;
			} else {
				gmState = GM_CLEAR_REQUEST;
			}
			} break;
		case GM_DELETE: {
			// We are done with the job. Propagate any remaining updates
			// to the schedd, then delete this object.
			DoneWithJob();
			// This object will be deleted when the update occurs
			} break;
		case GM_CLEAR_REQUEST: {
			// Remove all knowledge of any previous or present job
			// submission, in both the gridmanager and the schedd.

			// For now, put problem jobs on hold instead of
			// forgetting about current submission and trying again.
			// TODO: Let our action here be dictated by the user preference
			// expressed in the job ad.
			if( remoteJobId.cluster != 0 ) {
				sprintf( errorString, "Internal error: Attempting to clear "
									 "request, but remoteJobId.cluster(%d) "
									 "!= 0, condorState is %s (%d)",
									 remoteJobId.cluster,
									 getJobStatusString(condorState), 
									 condorState );
				gmState = GM_HOLD;
				break;
			}
			errorString = "";
			SetRemoteJobId( NULL );
			myResource->CancelSubmit( this );
			if ( newRemoteStatusAd != NULL ) {
				delete newRemoteStatusAd;
				newRemoteStatusAd = NULL;
			}
			doActivePoll = false;
			JobIdle();
			if ( submitLogged ) {
				JobEvicted();
			}
			UpdateJobLeaseSent( -1 );

			// If there are no updates to be done when we first enter this
			// state, requestScheddUpdate will return done immediately
			// and not waste time with a needless connection to the
			// schedd. If updates need to be made, they won't show up in
			// schedd_actions after the first pass through this state
			// because we modified our local variables the first time
			// through. However, since we registered update events the
			// first time, requestScheddUpdate won't return done until
			// they've been committed to the schedd.
			const char *name;
			ExprTree *expr;
			jobAd->ResetExpr();
			if ( jobAd->NextDirtyExpr(name, expr) ) {
				requestScheddUpdate( this, true );
				break;
			}
			lastProxyExpireTime = 0;
			lastProxyRefreshAttempt = 0;
			submitLogged = false;
			executeLogged = false;
			submitFailedLogged = false;
			terminateLogged = false;
			abortLogged = false;
			evictLogged = false;
			gmState = GM_UNSUBMITTED;
			remoteState = JOB_STATE_UNSUBMITTED;
			SetRemoteJobStatus( NULL );
			} break;
		case GM_HOLD: {
			// Put the job on hold in the schedd.
			// TODO: what happens if we learn here that the job is removed?

			// If the condor state is already HELD, then someone already
			// HELD it, so don't update anything else.
			if ( condorState != HELD ) {

				// Set the hold reason as best we can
				// TODO: set the hold reason in a more robust way.
				char holdReason[1024];
				holdReason[0] = '\0';
				holdReason[sizeof(holdReason)-1] = '\0';
				jobAd->LookupString( ATTR_HOLD_REASON, holdReason,
								  sizeof(holdReason) - 1 );
				if ( holdReason[0] == '\0' && errorString != "" ) {
					strncpy( holdReason, errorString.c_str(),
							 sizeof(holdReason) - 1 );
				}
				if ( holdReason[0] == '\0' ) {
					strncpy( holdReason, "Unspecified gridmanager error",
							 sizeof(holdReason) - 1 );
				}

				JobHeld( holdReason );
			}
			gmState = GM_DELETE;
			} break;
		default:
			EXCEPT( "(%d.%d) Unknown gmState %d!", procID.cluster,procID.proc,
					gmState );
		}

		if ( gmState != old_gm_state || remoteState != old_remote_state ) {
			reevaluate_state = true;
		}
		if ( remoteState != old_remote_state ) {
//			dprintf(D_FULLDEBUG, "(%d.%d) remote state change: %s -> %s\n",
//					procID.cluster, procID.proc,
//					JobStatusNames(old_remote_state),
//					JobStatusNames(remoteState));
			enteredCurrentRemoteState = time(NULL);
		}
		if ( gmState != old_gm_state ) {
			dprintf(D_FULLDEBUG, "(%d.%d) gm state change: %s -> %s\n",
					procID.cluster, procID.proc, GMStateNames[old_gm_state],
					GMStateNames[gmState]);
			enteredCurrentGmState = time(NULL);
			// If we were waiting for a pending gahp call, we're not
			// anymore so purge it.
			if ( gahp ) {
				gahp->purgePendingRequests();
			}
			// If we were calling a gahp func that used gahpAd, we're done
			// with it now, so free it.
			if ( gahpAd ) {
				delete gahpAd;
				gahpAd = NULL;
			}
			connectFailureCount = 0;
			resourcePingComplete = false;
		}

	} while ( reevaluate_state );

	if ( connect_failure && !resourceDown ) {
		if ( connectFailureCount < maxConnectFailures ) {
			connectFailureCount++;
			int retry_secs = param_integer(
				"GRIDMANAGER_CONNECT_FAILURE_RETRY_INTERVAL",5);
			dprintf(D_FULLDEBUG,
				"(%d.%d) Connection failure (try #%d), retrying in %d secs\n",
				procID.cluster,procID.proc,connectFailureCount,retry_secs);
			daemonCore->Reset_Timer( evaluateStateTid, retry_secs );
		} else {
			dprintf(D_FULLDEBUG,
				"(%d.%d) Connection failure, requesting a ping of the resource\n",
				procID.cluster,procID.proc);
			RequestPing();
		}
	}
}

void CondorJob::SetRemoteJobId( const char *job_id )
{
	int rc;
	std::string full_job_id;

	if ( job_id ) {
		rc = sscanf( job_id, "%d.%d", &remoteJobId.cluster,
					 &remoteJobId.proc );
		if ( rc != 2 ) {
			dprintf( D_ALWAYS,
					 "(%d.%d.) SetRemoteJobId: malformed job id: %s\n",
					 procID.cluster, procID.proc, job_id );
			return;
		}

		sprintf( full_job_id, "condor %s %s %s", remoteScheddName,
							 remotePoolName, job_id );
	} else {
		remoteJobId.cluster = 0;
	}

	BaseJob::SetRemoteJobId( full_job_id.c_str() );
}

void CondorJob::NotifyNewRemoteStatus( ClassAd *update_ad )
{
	int tmp_int;
	if ( update_ad == NULL ) {
			// This job was missing from a collective status query. Trigger
			// a specific query to see what's wrong.
		dprintf( D_FULLDEBUG, "(%d.%d) Got NULL classad from CondorResource\n",
				 procID.cluster, procID.proc );
		doActivePoll = true;
		SetEvaluateState();
		return;
	}
	dprintf( D_FULLDEBUG, "(%d.%d) Got classad from CondorResource\n",
			 procID.cluster, procID.proc );
	if ( update_ad->LookupInteger( ATTR_SERVER_TIME, tmp_int ) == 0 ) {
		dprintf( D_ALWAYS, "(%d.%d) Ad from remote schedd has no %s\n",
				 procID.cluster, procID.proc, ATTR_SERVER_TIME );
		delete update_ad;
		return;
	}
	if ( newRemoteStatusAd != NULL && tmp_int <= newRemoteStatusServerTime ) {
		dprintf( D_ALWAYS, "(%d.%d) Ad from remote schedd is stale\n",
				 procID.cluster, procID.proc );
		delete update_ad;
		return;
	}
	if ( newRemoteStatusAd != NULL ) {
		delete newRemoteStatusAd;
	}
	newRemoteStatusAd = update_ad;
	newRemoteStatusServerTime = tmp_int;
	SetEvaluateState();
}

void CondorJob::ProcessRemoteAd( ClassAd *remote_ad )
{
	int new_remote_state;
	ExprTree *new_expr, *old_expr;

	int index;
	const char *default_attrs_to_copy[] = {
		ATTR_BYTES_SENT,
		ATTR_BYTES_RECVD,
		ATTR_COMPLETION_DATE,
		ATTR_JOB_RUN_COUNT,
		ATTR_JOB_START_DATE,
		ATTR_ON_EXIT_BY_SIGNAL,
		ATTR_ON_EXIT_SIGNAL,
		ATTR_ON_EXIT_CODE,
		ATTR_EXIT_REASON,
		ATTR_JOB_CURRENT_START_DATE,
		ATTR_JOB_LOCAL_SYS_CPU,
		ATTR_JOB_LOCAL_USER_CPU,
		ATTR_JOB_REMOTE_SYS_CPU,
		ATTR_JOB_REMOTE_USER_CPU,
		ATTR_NUM_CKPTS,
		ATTR_NUM_GLOBUS_SUBMITS,
		ATTR_NUM_JOB_STARTS,
		ATTR_NUM_JOB_RECONNECTS,
		ATTR_NUM_SHADOW_EXCEPTIONS,
		ATTR_NUM_SHADOW_STARTS,
		ATTR_NUM_MATCHES,
		ATTR_NUM_RESTARTS,
		ATTR_JOB_REMOTE_WALL_CLOCK,
		ATTR_JOB_CORE_DUMPED,
		ATTR_EXECUTABLE_SIZE,
		ATTR_IMAGE_SIZE,
		ATTR_SPOOLED_OUTPUT_FILES,
		NULL };		// list must end with a NULL

	if ( remote_ad == NULL ) {
		return;
	}

	char **attrs_to_copy;
	char *config_attrs_to_copy = param("CONDORC_ATTRS_TO_COPY");
	bool freeAttrs = false;

	if (config_attrs_to_copy == NULL) {
		// use the defaults
		attrs_to_copy = const_cast<char **>(default_attrs_to_copy);
	} else {
		StringList sl(NULL, ", ");
		freeAttrs = true;
		sl.initializeFromString(config_attrs_to_copy);
		sl.rewind();
		attrs_to_copy = new char *[sl.number() + 1];
		for (int i = 0; i < sl.number(); i++) {
			std::string attribute(sl.next());
			attrs_to_copy[i] = new char[ attribute.length() + 1 ];
			strcpy(attrs_to_copy[i], attribute.c_str());
		}
		attrs_to_copy[sl.number()] = NULL;
		free(config_attrs_to_copy);
	}

	dprintf( D_FULLDEBUG, "(%d.%d) Processing remote job status ad\n",
			 procID.cluster, procID.proc );

	remote_ad->LookupInteger( ATTR_JOB_STATUS, new_remote_state );

	if ( new_remote_state == IDLE ) {
		JobIdle();
	}
	if ( new_remote_state == RUNNING || new_remote_state == TRANSFERRING_OUTPUT ) {
		JobRunning();
	}
	// If the job has been removed locally, don't propagate a hold from
	// the remote schedd. 
	// If HELD is the first job status we get from the remote schedd,
	// assume that it's an old hold that was also reflected in the local
	// schedd and has since been released locally (and should be released
	// remotely as well). This won't always be true, but releasing the
	// remote job anyway shouldn't cause any major trouble.
	if ( new_remote_state == HELD && condorState != REMOVED &&
		 remoteState != JOB_STATE_UNKNOWN ) {
		char *reason = NULL;
		int code = 0;
		int subcode = 0;
		if ( remote_ad->LookupString( ATTR_HOLD_REASON, &reason ) ) {
			remote_ad->LookupInteger( ATTR_HOLD_REASON_CODE, code );
			remote_ad->LookupInteger( ATTR_HOLD_REASON_SUBCODE, subcode );
			JobHeld( reason, code, subcode );
			free( reason );
		} else {
			JobHeld( "held remotely with no hold reason" );
		}
	}
	remoteState = new_remote_state;
	SetRemoteJobStatus( getJobStatusString( remoteState ) );


	index = -1;
	while ( attrs_to_copy[++index] != NULL ) {
		old_expr = jobAd->LookupExpr( attrs_to_copy[index] );
		new_expr = remote_ad->LookupExpr( attrs_to_copy[index] );

		if ( new_expr != NULL && ( old_expr == NULL || !(*old_expr == *new_expr) ) ) {
			jobAd->Insert( attrs_to_copy[index], new_expr->Copy() );
		}
	}

	if (freeAttrs) {
		int i = 0;
		while (attrs_to_copy[i] != NULL) {
			free(attrs_to_copy[i]);
			i++;
		}
		delete [] attrs_to_copy;
	}

	requestScheddUpdate( this, false );

	return;
}

BaseResource *CondorJob::GetResource()
{
	return (BaseResource *)myResource;
}

// New black-list version
ClassAd *CondorJob::buildSubmitAd()
{
	int now = time(NULL);
	std::string expr;
	ClassAd *submit_ad;
	ExprTree *next_expr;
	int tmp_int;

		// Base the submit ad on our own job ad
	submit_ad = new ClassAd( *jobAd );

	submit_ad->Delete( ATTR_CLUSTER_ID );
	submit_ad->Delete( ATTR_PROC_ID );
	submit_ad->Delete( ATTR_USER );
	submit_ad->Delete( ATTR_OWNER );
	submit_ad->Delete( ATTR_GRID_RESOURCE );
	submit_ad->Delete( ATTR_JOB_MATCHED );
	submit_ad->Delete( ATTR_JOB_MANAGED );
	submit_ad->Delete( ATTR_STAGE_IN_FINISH );
	submit_ad->Delete( ATTR_STAGE_IN_START );
	submit_ad->Delete( ATTR_SCHEDD_BIRTHDATE );
	submit_ad->Delete( ATTR_FILE_SYSTEM_DOMAIN );
	submit_ad->Delete( ATTR_ULOG_FILE );
	submit_ad->Delete( ATTR_ULOG_USE_XML );
	submit_ad->Delete( ATTR_NOTIFY_USER );
	submit_ad->Delete( ATTR_ON_EXIT_HOLD_CHECK );
	submit_ad->Delete( ATTR_ON_EXIT_REMOVE_CHECK );
	submit_ad->Delete( ATTR_PERIODIC_HOLD_CHECK );
	submit_ad->Delete( ATTR_PERIODIC_RELEASE_CHECK );
	submit_ad->Delete( ATTR_PERIODIC_REMOVE_CHECK );
	submit_ad->Delete( ATTR_SERVER_TIME );
	submit_ad->Delete( ATTR_JOB_MANAGED );
	submit_ad->Delete( ATTR_GLOBAL_JOB_ID );
	submit_ad->Delete( "CondorPlatform" );
	submit_ad->Delete( "CondorVersion" );
	submit_ad->Delete( ATTR_WANT_CLAIMING );
	submit_ad->Delete( ATTR_WANT_MATCHING );
	submit_ad->Delete( ATTR_HOLD_REASON );
	submit_ad->Delete( ATTR_HOLD_REASON_CODE );
	submit_ad->Delete( ATTR_HOLD_REASON_SUBCODE );
	submit_ad->Delete( ATTR_LAST_HOLD_REASON );
	submit_ad->Delete( ATTR_LAST_HOLD_REASON_CODE );
	submit_ad->Delete( ATTR_LAST_HOLD_REASON_SUBCODE );
	submit_ad->Delete( ATTR_RELEASE_REASON );
	submit_ad->Delete( ATTR_LAST_RELEASE_REASON );
	submit_ad->Delete( ATTR_JOB_STATUS_ON_RELEASE );
	submit_ad->Delete( ATTR_LAST_JOB_LEASE_RENEWAL );
	submit_ad->Delete( ATTR_JOB_LEASE_DURATION );
	submit_ad->Delete( ATTR_LAST_JOB_LEASE_RENEWAL_FAILED );
	submit_ad->Delete( ATTR_TIMER_REMOVE_CHECK );
	submit_ad->Delete( ATTR_JOB_LEASE_EXPIRATION );
	submit_ad->Delete( ATTR_AUTO_CLUSTER_ID );
	submit_ad->Delete( ATTR_AUTO_CLUSTER_ATTRS );

	submit_ad->Assign( ATTR_JOB_STATUS, HELD );
	submit_ad->Assign( ATTR_HOLD_REASON, "Spooling input data files" );
	submit_ad->Assign( ATTR_JOB_UNIVERSE, CONDOR_UNIVERSE_VANILLA );

	submit_ad->Assign( ATTR_Q_DATE, now );
	submit_ad->Assign( ATTR_CURRENT_HOSTS, 0 );
	submit_ad->Assign( ATTR_COMPLETION_DATE, 0 );
	submit_ad->Assign( ATTR_JOB_REMOTE_WALL_CLOCK, (float)0.0 );
	submit_ad->Assign( ATTR_JOB_LOCAL_USER_CPU, (float)0.0 );
	submit_ad->Assign( ATTR_JOB_LOCAL_SYS_CPU, (float)0.0 );
	submit_ad->Assign( ATTR_JOB_REMOTE_USER_CPU, (float)0.0 );
	submit_ad->Assign( ATTR_JOB_REMOTE_SYS_CPU, (float)0.0 );
	submit_ad->Assign( ATTR_JOB_EXIT_STATUS, 0 );
	submit_ad->Assign( ATTR_NUM_CKPTS, 0 );
	submit_ad->Assign( ATTR_NUM_RESTARTS, 0 );
	submit_ad->Assign( ATTR_NUM_SYSTEM_HOLDS, 0 );
	submit_ad->Assign( ATTR_JOB_COMMITTED_TIME, 0 );
	submit_ad->Assign( ATTR_COMMITTED_SLOT_TIME, 0 );
	submit_ad->Assign( ATTR_CUMULATIVE_SLOT_TIME, 0 );
	submit_ad->Assign( ATTR_TOTAL_SUSPENSIONS, 0 );
	submit_ad->Assign( ATTR_LAST_SUSPENSION_TIME, 0 );
	submit_ad->Assign( ATTR_CUMULATIVE_SUSPENSION_TIME, 0 );
	submit_ad->Assign( ATTR_COMMITTED_SUSPENSION_TIME, 0 );
	submit_ad->Assign( ATTR_ON_EXIT_BY_SIGNAL, false );
	submit_ad->Assign( ATTR_ENTERED_CURRENT_STATUS, now  );
	submit_ad->Assign( ATTR_JOB_NOTIFICATION, NOTIFY_NEVER );

		// If stdout or stderr is not in the job's Iwd, rename them and
		// add a transfer remap. Otherwise, the file transfer object will
		// place them in the Iwd when we stage back the job's output files.
	std::string output_remaps = "";
	std::string filename = "";
	submit_ad->LookupString( ATTR_TRANSFER_OUTPUT_REMAPS, output_remaps );

	jobAd->LookupString( ATTR_JOB_OUTPUT, filename );
	if ( strcmp( filename.c_str(), condor_basename( filename.c_str() ) ) &&
		 !nullFile( filename.c_str() ) ) {

		char const *working_name = "_condor_stdout";
		if ( !output_remaps.empty() ) output_remaps += ";";
		sprintf_cat( output_remaps, "%s=%s", working_name, filename.c_str() );
		submit_ad->Assign( ATTR_JOB_OUTPUT, working_name );
	}

	jobAd->LookupString( ATTR_JOB_ERROR, filename );
	if ( strcmp( filename.c_str(), condor_basename( filename.c_str() ) ) &&
		 !nullFile( filename.c_str() ) ) {

		char const *working_name = "_condor_stderr";
		if ( !output_remaps.empty() ) output_remaps += ";";
		sprintf_cat( output_remaps, "%s=%s", working_name, filename.c_str() );
		submit_ad->Assign( ATTR_JOB_ERROR, working_name );
	}

	if ( !output_remaps.empty() ) {
		submit_ad->Assign( ATTR_TRANSFER_OUTPUT_REMAPS,
						   output_remaps.c_str() );
	}

	sprintf( expr, "%s = %s == %d", ATTR_JOB_LEAVE_IN_QUEUE, ATTR_JOB_STATUS,
				  COMPLETED );

	if ( jobAd->LookupInteger( ATTR_JOB_LEASE_EXPIRATION, tmp_int ) ) {
		submit_ad->Assign( ATTR_TIMER_REMOVE_CHECK, tmp_int );
		sprintf_cat( expr, " && ( time() < %s )", ATTR_TIMER_REMOVE_CHECK );
	}

	submit_ad->Insert( expr.c_str() );

	sprintf( expr, "%s = Undefined", ATTR_OWNER );
	submit_ad->Insert( expr.c_str() );

	const int STAGE_IN_TIME_LIMIT  = 60 * 60 * 8; // 8 hours in seconds.
	sprintf( expr, "%s = (%s > 0) =!= True && time() > %s + %d",
				  ATTR_PERIODIC_REMOVE_CHECK, ATTR_STAGE_IN_FINISH,
				  ATTR_Q_DATE, STAGE_IN_TIME_LIMIT );
	submit_ad->Insert( expr.c_str() );

	submit_ad->Assign( ATTR_SUBMITTER_ID, submitterId );

		// If JOB_PROXY_OVERRIDE_FILE is set in the config file, then
		// these attributes aren't set in the local job ad, so we need
		// to set them explicitly.
	if ( jobProxy ) {
		submit_ad->Assign( ATTR_X509_USER_PROXY, jobProxy->proxy_filename );
		submit_ad->Assign( ATTR_X509_USER_PROXY_SUBJECT,
						   jobProxy->subject->subject_name );
		if ( jobProxy->subject->has_voms_attrs ) {
			submit_ad->Assign( ATTR_X509_USER_PROXY_FQAN,
							   jobProxy->subject->fqan );
		}
	}

	bool cleared_environment = false;
	bool cleared_arguments = false;

		// Remove all remote_* attributes from the new ad before
		// translating remote_* attributes from the original ad.
		// See gittrac #376 for why we have two loops here.
	const char *next_name;
	submit_ad->ResetName();
	while ( (next_name = submit_ad->NextNameOriginal()) != NULL ) {
		if ( strncasecmp( next_name, "REMOTE_", 7 ) == 0 &&
			 strlen( next_name ) > 7 ) {

			submit_ad->Delete( next_name );
		}
	}

	jobAd->ResetExpr();
	while ( jobAd->NextExpr(next_name, next_expr) ) {
		if ( strncasecmp( next_name, "REMOTE_", 7 ) == 0 &&
			 strlen( next_name ) > 7 ) {

			char const *attr_name = &(next_name[7]);

			if(strcasecmp(attr_name,ATTR_JOB_ENVIRONMENT1) == 0 ||
			   strcasecmp(attr_name,ATTR_JOB_ENVIRONMENT1_DELIM) == 0 ||
			   strcasecmp(attr_name,ATTR_JOB_ENVIRONMENT2) == 0)
			{
				//Any remote environment settings indicate that we
				//should clear whatever environment was already copied
				//over from the non-remote settings, so the non-remote
				//settings can never trump the remote settings.
				if(!cleared_environment) {
					cleared_environment = true;
					submit_ad->Delete(ATTR_JOB_ENVIRONMENT1);
					submit_ad->Delete(ATTR_JOB_ENVIRONMENT1_DELIM);
					submit_ad->Delete(ATTR_JOB_ENVIRONMENT2);
				}
			}

			if(strcasecmp(attr_name,ATTR_JOB_ARGUMENTS1) == 0 ||
			   strcasecmp(attr_name,ATTR_JOB_ARGUMENTS2) == 0)
			{
				//Any remote arguments settings indicate that we
				//should clear whatever arguments was already copied
				//over from the non-remote settings, so the non-remote
				//settings can never trump the remote settings.
				if(!cleared_arguments) {
					cleared_arguments = true;
					submit_ad->Delete(ATTR_JOB_ARGUMENTS1);
					submit_ad->Delete(ATTR_JOB_ARGUMENTS2);
				}
			}

			submit_ad->Insert( attr_name, next_expr->Copy() );
		}
	}

	// If the remote job will use a shadow, we always want to set
	// JobLeaseDuration. Otherwise, starter-shadow reconnect is disabled.
	// We need to be careful to respect the user's setting of the
	// attribute, hence why we do this check last.
	tmp_int = CONDOR_UNIVERSE_VANILLA;
	submit_ad->LookupInteger( ATTR_JOB_UNIVERSE, tmp_int );
	if ( universeCanReconnect( tmp_int ) &&
		 submit_ad->Lookup( ATTR_JOB_LEASE_DURATION ) == NULL ) {

		submit_ad->Assign( ATTR_JOB_LEASE_DURATION, 20 * 60 );
	}

		// worry about ATTR_JOB_[OUTPUT|ERROR]_ORIG

	return submit_ad;
}

ClassAd *CondorJob::buildStageInAd()
{
	ClassAd *stage_in_ad;

		// Base the stage in ad on our own job ad
//	stage_in_ad = new ClassAd( *jobAd );
	stage_in_ad = buildSubmitAd();

	stage_in_ad->Assign( ATTR_CLUSTER_ID, remoteJobId.cluster );
	stage_in_ad->Assign( ATTR_PROC_ID, remoteJobId.proc );

	stage_in_ad->Delete( ATTR_ULOG_FILE );

	return stage_in_ad;
}