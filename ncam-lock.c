#define MODULE_LOG_PREFIX "lock"

#include "globals.h"
#include "ncam-lock.h"
#include "ncam-time.h"

extern char *LOG_LIST;

static void rwlock_timeout_log(CS_MUTEX_LOCK *l, int8_t type)
{
#ifdef WITH_DEBUG
	if(l->name != LOG_LIST)
		{ cs_log("WARNING lock %s (%s) wait timeout; continuing to wait.", l->name ? l->name : "<destroying>", type == WRITELOCK ? "WRITELOCK" : "READLOCK"); }
#else
	(void)l;
	(void)type;
#endif
}

static int8_t rwlock_wait_locked(CS_MUTEX_LOCK *l, int8_t type, int log_errors)
{
	const uint32_t wait_ms = l->timeout ? (uint32_t)l->timeout * 1000U : 1000U;
	while((type == WRITELOCK && (l->writelock || l->readlock)) ||
	      (type == READLOCK && (l->writelock || l->waiting_writers)))
	{
		if(l->destroying || l->flag)
			{ return 0; }
		struct timespec ts;
		add_ms_to_timespec(&ts, (int32_t)wait_ms);
		int32_t rc = pthread_cond_timedwait(type == WRITELOCK ? &l->writecond : &l->readcond, &l->lock, &ts);
		if(rc == ETIMEDOUT)
		{
			rwlock_timeout_log(l, type);
		}
		else if(rc != 0 && log_errors)
		{
			cs_log("ERROR: pthread_cond_timedwait on lock %s failed: %d %s", l->name ? l->name : "<destroying>", rc, strerror(rc));
		}
	}
	return !(l->destroying || l->flag);
}

static void rwlock_acquire(const char *n, CS_MUTEX_LOCK *l, int8_t type, int log_errors)
{
	if(!l || !l->name || l->flag)
		{ return; }

	if(log_errors) { SAFE_MUTEX_LOCK_R(&l->lock, n); }
	else { SAFE_MUTEX_LOCK_NOLOG_R(&l->lock, n); }

	if(l->destroying || l->flag || !l->name)
	{
		if(log_errors) { SAFE_MUTEX_UNLOCK_R(&l->lock, n); }
		else { SAFE_MUTEX_UNLOCK_NOLOG_R(&l->lock, n); }
		return;
	}

	l->users++;
	int8_t acquired = 0;
	if(type == WRITELOCK)
	{
		l->waiting_writers++;
		acquired = rwlock_wait_locked(l, type, log_errors);
		l->waiting_writers--;
		if(acquired)
		{ l->writelock = 1; }
	}
	else
	{
		acquired = rwlock_wait_locked(l, type, log_errors);
		if(acquired)
		{ l->readlock++; }
	}

	if(!acquired)
	{
		l->users--;
		if(l->destroying && l->users == 0)
			{ SAFE_COND_SIGNAL_R(&l->destroycond, n); }
	}

	if(log_errors) { SAFE_MUTEX_UNLOCK_R(&l->lock, n); }
	else { SAFE_MUTEX_UNLOCK_NOLOG_R(&l->lock, n); }
}

void cs_lock_create(const char *n, CS_MUTEX_LOCK *l, const char *name, uint32_t timeout_ms)
{
	memset(l, 0, sizeof(CS_MUTEX_LOCK));
	l->timeout = timeout_ms / 1000;
	if(timeout_ms && !l->timeout) { l->timeout = 1; }
	l->name = name;
	SAFE_MUTEX_INIT_R(&l->lock, NULL, n);
	__cs_pthread_cond_init(n, &l->writecond);
	__cs_pthread_cond_init(n, &l->readcond);
	__cs_pthread_cond_init(n, &l->destroycond);
#ifdef WITH_MUTEXDEBUG
	cs_log_dbg(D_TRACE, "lock %s created", name);
#endif
}

void cs_lock_create_nolog(const char *n, CS_MUTEX_LOCK *l, const char *name, uint32_t timeout_ms)
{
	memset(l, 0, sizeof(CS_MUTEX_LOCK));
	l->timeout = timeout_ms / 1000;
	if(timeout_ms && !l->timeout) { l->timeout = 1; }
	l->name = name;
	SAFE_MUTEX_INIT_NOLOG_R(&l->lock, NULL, n);
	__cs_pthread_cond_init(n, &l->writecond);
	__cs_pthread_cond_init(n, &l->readcond);
	__cs_pthread_cond_init(n, &l->destroycond);
#ifdef WITH_MUTEXDEBUG
	cs_log_dbg(D_TRACE, "lock %s created", name);
#endif
}

void cs_lock_destroy(const char *pn, CS_MUTEX_LOCK *l)
{
	if(!l || !l->name || l->flag) { return; }

	SAFE_MUTEX_LOCK_R(&l->lock, pn);
#ifdef WITH_DEBUG
	const char *old_name = l->name;
#else
	const char *old_name = NULL;
#endif
	l->destroying = 1;
	l->name = NULL;
	SAFE_COND_BROADCAST_R(&l->writecond, pn);
	SAFE_COND_BROADCAST_R(&l->readcond, pn);
	while(l->users > 0)
	{
		int32_t rc = pthread_cond_wait(&l->destroycond, &l->lock);
		if(rc != 0)
			{ break; }
	}
	l->flag = 1;
	SAFE_MUTEX_UNLOCK_R(&l->lock, pn);

#ifdef WITH_DEBUG
	if(old_name && old_name != LOG_LIST)
		{ cs_log_dbg(D_TRACE, "lock %s destroyed", old_name); }
#endif
	pthread_mutex_destroy(&l->lock);
	pthread_cond_destroy(&l->writecond);
	pthread_cond_destroy(&l->readcond);
	pthread_cond_destroy(&l->destroycond);
}

void cs_rwlock_int(const char *n, CS_MUTEX_LOCK *l, int8_t type)
{
	rwlock_acquire(n, l, type, 1);
}

void cs_rwlock_int_nolog(const char *n, CS_MUTEX_LOCK *l, int8_t type)
{
	rwlock_acquire(n, l, type, 0);
}

void cs_rwunlock_int(const char *n, CS_MUTEX_LOCK *l, int8_t type)
{
	if(!l || l->flag) { return; }
	SAFE_MUTEX_LOCK_R(&l->lock, n);
	if(type == WRITELOCK)
	{
		if(l->writelock > 0) { l->writelock = 0; }
		if(l->waiting_writers)
			{ SAFE_COND_SIGNAL_R(&l->writecond, n); }
		else
			{ SAFE_COND_BROADCAST_R(&l->readcond, n); }
		if(l->users > 0) { l->users--; }
		if(l->destroying && l->users == 0)
			{ SAFE_COND_SIGNAL_R(&l->destroycond, n); }
	}
	else
	{
		if(l->readlock > 0) { l->readlock--; }
		if(l->readlock == 0 && l->waiting_writers)
			{ SAFE_COND_SIGNAL_R(&l->writecond, n); }
		else if(!l->waiting_writers)
			{ SAFE_COND_BROADCAST_R(&l->readcond, n); }
		if(l->users > 0) { l->users--; }
		if(l->destroying && l->users == 0)
			{ SAFE_COND_SIGNAL_R(&l->destroycond, n); }
	}
	SAFE_MUTEX_UNLOCK_R(&l->lock, n);
}

void cs_rwunlock_int_nolog(const char *n, CS_MUTEX_LOCK *l, int8_t type)
{
	if(!l || l->flag) { return; }
	SAFE_MUTEX_LOCK_NOLOG_R(&l->lock, n);
	if(type == WRITELOCK)
	{
		if(l->writelock > 0) { l->writelock = 0; }
		if(l->waiting_writers)
			{ SAFE_COND_SIGNAL_R(&l->writecond, n); }
		else
			{ SAFE_COND_BROADCAST_R(&l->readcond, n); }
	}
	else
	{
		if(l->readlock > 0) { l->readlock--; }
		if(l->readlock == 0 && l->waiting_writers)
			{ SAFE_COND_SIGNAL_R(&l->writecond, n); }
		else if(!l->waiting_writers)
			{ SAFE_COND_BROADCAST_R(&l->readcond, n); }
	}
	if(l->users > 0) { l->users--; }
	if(l->destroying && l->users == 0)
		{ SAFE_COND_SIGNAL_R(&l->destroycond, n); }
	SAFE_MUTEX_UNLOCK_NOLOG_R(&l->lock, n);
}

int8_t cs_try_rwlock_int(const char *n, CS_MUTEX_LOCK *l, int8_t type)
{
	if(!l || l->flag)
		{ return 1; }
	int8_t status = 0;
	SAFE_MUTEX_LOCK_R(&l->lock, n);
	if(l->destroying || !l->name)
	{
		SAFE_MUTEX_UNLOCK_R(&l->lock, n);
		return 1;
	}
	l->users++;
	if(type == WRITELOCK)
	{
		if(l->writelock || l->readlock || l->waiting_writers)
			{ status = 1; }
		else
			{ l->writelock = 1; }
	}
	else
	{
		if(l->writelock || l->waiting_writers)
			{ status = 1; }
		else
			{ l->readlock++; }
	}
	if(status)
	{
		if(l->users > 0) { l->users--; }
	}
	SAFE_MUTEX_UNLOCK_R(&l->lock, n);
	return status;
}
