
#include "postgres.h"

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "access/hash.h"
#include "utils/hashutils.h"

/* these headers are used by this particular worker's code */
#include "kv_shm.h"


void KVManageWork(Datum);
void LaunchBackgroundManager(void);
void KVDoWork(Datum);
pid_t LaunchBackgroundWorker(WorkerProcOid *workerOid,
                             ManagerSharedMem *manager);
void ShutOffBackgroundWorker(WorkerProcOid *workerOid,
                             ManagerSharedMem *manager);


/* non-shared hash can be enlarged */
static long HASHSIZE = 1;

/* reference by manager process */
static HTAB *workerHash = NULL;

typedef struct WorkerProcData {
    pid_t pid;
    WorkerProcOid oid;
    BackgroundWorkerHandle *handle;
} WorkerProcData;

/* flags set by signal handlers */
static volatile sig_atomic_t gotSIGTERM = false;



/*
 * Signal handler for SIGTERM
 * Set a flag to let the main loop to terminate, and set our latch to wake it up.
 */
static void KVSIGTERM(SIGNAL_ARGS) {
    int save_errno = errno;

    gotSIGTERM = true;
    SetLatch(MyLatch);

    errno = save_errno;
}

/*
 * Compare function for WorkerProcOid
 */
int CompareWorkerProcOid(const void *key1, const void *key2, Size keysize) {
    const WorkerProcOid *k1 = (const WorkerProcOid *) key1;
    const WorkerProcOid *k2 = (const WorkerProcOid *) key2;

    if (k1 == NULL || k2 == NULL) {
        return -1;
    }

    if (k1->relationId == k2->relationId &&
        k1->databaseId == k2->databaseId) {
        return 0;
    }

    return -1;
}

/*
 * Hash function for WorkerProcOid
 */
uint32 HashWorkerProcOid(const void *key, Size keysize) {
    const WorkerProcOid *k = (const WorkerProcOid *) key;
    uint32 s = hash_combine(0, hash_uint32(k->databaseId));
    return hash_combine(s, hash_uint32(k->relationId));
}

/*
 * Initialize shared memory and release it when exits
 */
void KVManageWork(Datum arg) {
    /* Establish signal handlers before unblocking signals. */
    /* pqsignal(SIGTERM, KVSIGTERM); */
    /*
     * We on purpose do not use pqsignal due to its setting at flags = restart.
     * With the setting, the process cannot exit on sem_wait.
     */
    struct sigaction act;
    act.sa_handler = KVSIGTERM;
    act.sa_flags = 0;
    sigaction(SIGTERM, &act, NULL);

    /* We're now ready to receive signals */
    BackgroundWorkerUnblockSignals();

    ManagerSharedMem *manager = InitManagerSharedMem();

    HASHCTL hash_ctl;
    memset(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(WorkerProcOid);
    hash_ctl.entrysize = sizeof(WorkerProcData);
    hash_ctl.match = CompareWorkerProcOid;
    hash_ctl.hash = HashWorkerProcOid;
    workerHash = hash_create("workerHash",
                             HASHSIZE,
                             &hash_ctl,
                             HASH_ELEM | HASH_COMPARE | HASH_FUNCTION);

    while (!gotSIGTERM) {
        /*
         * Don't create worker process until needed!
         * Semaphore here also catches SIGTERN signal.
         */
        if (SemWait(&manager->manager, __func__) == -1) {
            break;
        }

        WorkerProcOid workerOid;
        workerOid.databaseId = manager->databaseId;
        workerOid.relationId = manager->relationId;
        FuncName func = manager->func;

        switch (func) {
            case OPEN:
                LaunchBackgroundWorker(&workerOid, manager);
                break;
            case CLOSE:
                ShutOffBackgroundWorker(&workerOid, manager);
                break;
            default:
                ereport(ERROR, (errmsg("%s failed in switch", __func__)));
        }

        SemPost(&manager->backend, __func__);
    };

    HASH_SEQ_STATUS status;
    hash_seq_init(&status, workerHash);
    WorkerProcData *entry = NULL;
    while ((entry = hash_seq_search(&status)) != NULL) {
        ShutOffBackgroundWorker(&entry->oid, manager);
    }

    CloseManagerSharedMem(manager);
    proc_exit(0);
}

/*
 * Entrypoint, register manager process here, called in _PG_init
 */
void LaunchBackgroundManager(void) {
    printf("\n~~~~~~~~~~~~%s~~~~~~~~~~~~~~~\n", __func__);

    if (!process_shared_preload_libraries_in_progress) {
        return;
    }

    BackgroundWorker worker;
    memset(&worker, 0, sizeof(worker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "KV manager");
    snprintf(worker.bgw_type, BGW_MAXLEN, "KV manager");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 1;
    sprintf(worker.bgw_library_name, "kv_fdw");
    sprintf(worker.bgw_function_name, "KVManageWork");
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);
}

void KVDoWork(Datum arg) {
    WorkerProcOid workerOid;
    workerOid.databaseId = DatumGetObjectId(arg);
    workerOid.relationId = *((Oid *) (MyBgworkerEntry->bgw_extra));
    KVWorkerMain(&workerOid);
    proc_exit(0);
}

/*
 * Entrypoint, dynamically register worker process here, at most one process
 * for each foreign table created by kv engine.
 */
pid_t LaunchBackgroundWorker(WorkerProcOid *workerOid,
                             ManagerSharedMem *manager) {
    printf("\n~~~~~~~~~~~~%s~~~~~~~~~~~~~~~\n", __func__);

    /* check whether the requested relation has the process */
    bool found = false;
    WorkerProcData *entry = hash_search(workerHash, workerOid, HASH_ENTER, &found);
    if (found) {  /* worker has already been created */
        return entry->pid;
    }

    BackgroundWorker worker;
    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    sprintf(worker.bgw_library_name, "kv_fdw");
    sprintf(worker.bgw_function_name, "KVDoWork");
    snprintf(worker.bgw_name, BGW_MAXLEN, "KV worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "KV worker");
    worker.bgw_main_arg = ObjectIdGetDatum(workerOid->databaseId);
    memcpy(worker.bgw_extra,
           (const char *) (&(workerOid->relationId)),
           sizeof(workerOid->relationId));
    /* set bgw_notify_pid so that we can use WaitForBackgroundWorkerStartup */
    worker.bgw_notify_pid = MyProcPid;
    BackgroundWorkerHandle *handle;
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) {
        return 0;
    }

    pid_t pid;
    BgwHandleStatus status = WaitForBackgroundWorkerStartup(handle, &pid);
    if (status == BGWH_STOPPED) {
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("could not start background process"),
                 errhint("More details may be available in the server log.")));
    }
    if (status == BGWH_POSTMASTER_DIED) {
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("cannot start background processes without postmaster"),
                 errhint("Kill all remaining database processes and restart "
                         "the database.")));
    }
    Assert(status == BGWH_STARTED);

    /*
     * Make sure worker process has inited to avoid race between
     * backend and worker.
     */
    SemWait(&manager->ready, __func__);
    entry->pid = pid;
    entry->oid = *workerOid;
    entry->handle = handle;

    return pid;
}

/*
 * Terminate the specified dynamically register worker process.
 */
void ShutOffBackgroundWorker(WorkerProcOid *workerOid,
                             ManagerSharedMem *manager) {
    printf("\n~~~~~~~~~~~~%s~~~~~~~~~~~~~~~\n", __func__);

    if (!OidIsValid(workerOid->relationId)) {
        /* terminate all workers related to the databse */
        HASH_SEQ_STATUS status;
        hash_seq_init(&status, workerHash);

        WorkerProcData *entry = NULL;
        while ((entry = hash_seq_search(&status)) != NULL) {
            if (entry->oid.databaseId != workerOid->databaseId) {
                continue;
            }

            TerminateWorker(&entry->oid);
            TerminateBackgroundWorker(entry->handle);
            WaitForBackgroundWorkerShutdown(entry->handle);
            pfree(entry->handle);
        }
    } else {
        /* check whether the requested relation has the process */
        bool found = false;
        WorkerProcData *entry = hash_search(workerHash,
                                            workerOid,
                                            HASH_REMOVE,
                                            &found);
        if (!found) {
            return;
        }

        TerminateWorker(workerOid);
        TerminateBackgroundWorker(entry->handle);
        WaitForBackgroundWorkerShutdown(entry->handle);
        pfree(entry->handle);
    }
}
