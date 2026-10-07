#define MODULE_LOG_PREFIX "failban"

#include "globals.h"
#include "module-anticasc.h"
#include "ncam-net.h"
#include "ncam-string.h"
#include "ncam-time.h"

static int32_t cs_check_v(IN_ADDR_T ip, int32_t port, int32_t add, char *info, int32_t acosc_penalty_duration)
{
	int32_t result = 0;
	if(!(cfg.failbantime || acosc_enabled()))
		return 0;
	if(add && !acosc_penalty_duration && cfg.failbantime <= 0)
		return 0;
	if(!cfg.v_list)
		cfg.v_list = ll_create("v_list");
	struct timeb now;
	cs_ftime(&now);
	LL_ITER itr = ll_iter_create(cfg.v_list);
	V_BAN *v = NULL;
	int32_t ftime = cfg.failbantime > 0 ? cfg.failbantime * 60 * 1000 : 0;
	while((v = ll_iter_next(&itr)))
	{
		int64_t gone = comp_timeb(&now, &v->v_time);
		if(((gone >= ftime) && !v->acosc_entry && ftime > 0) || (v->acosc_entry && (gone / 1000 >= v->acosc_penalty_dur)))
		{
			NULLFREE(v->info);
			ll_iter_remove_data(&itr);
			continue;
		}
		bool match = IP_EQUAL(ip, v->v_ip) && (port == v->v_port || v->v_port == 0);
		if(!match)
			continue;
		if(!add)
		{
			if(v->acosc_entry || (cfg.failbancount > 0 && v->v_count >= cfg.failbancount))
			{
				result = 1;
				if(!v->blocked_logged)
				{
					int64_t left = v->acosc_entry ? v->acosc_penalty_dur - (gone / 1000) : (ftime - gone) / 1000;
					cs_log("blocked %s for %" PRId64 " seconds", cs_inet_ntoa(v->v_ip), left > 0 ? left : 0);
					v->blocked_logged = true;
				}
				break;
			}
			continue;
		}
		result = 1;
		if(v->acosc_entry)
			return result;
		if(v->v_count < cfg.failbancount)
		{
			v->v_count++;
			if(v->v_count >= cfg.failbancount)
			{
				cs_ftime(&v->v_time);
				v->blocked_logged = true;
				cs_log("banned %s for %d minutes", cs_inet_ntoa(v->v_ip), cfg.failbantime);
			}
		}
		if(info && !v->info)
			v->info = cs_strdup(info);
		return result;
	}
	if(add && !result)
	{
		if(!cs_malloc(&v, sizeof(V_BAN)))
			return 0;
		cs_ftime(&v->v_time);
		v->v_ip = ip;
		v->v_port = port;
		v->v_count = 1;
		v->acosc_entry = false;
		v->acosc_penalty_dur = 0;
		v->blocked_logged = false;
		if(acosc_penalty_duration > 0)
		{
			v->v_count = cfg.failbancount + 1;
			v->acosc_entry = true;
			v->acosc_penalty_dur = acosc_penalty_duration;
			v->blocked_logged = false;
			cs_log("banned %s for %d seconds", cs_inet_ntoa(v->v_ip), acosc_penalty_duration);
		}
		else if(cfg.failbancount <= 1)
		{
			v->blocked_logged = false;
			cs_log("banned %s for %d minutes", cs_inet_ntoa(v->v_ip), cfg.failbantime);
		}
		if(info)
			v->info = cs_strdup(info);
		ll_iter_insert(&itr, v);
	}
	return result;
}

int32_t cs_check_violation(IN_ADDR_T ip, int32_t port)
{
	return cs_check_v(ip, port, 0, NULL, 0);
}

int32_t cs_add_violation_by_ip(IN_ADDR_T ip, int32_t port, char *info)
{
	return cs_check_v(ip, port, 1, info, 0);
}

int32_t cs_add_violation_by_ip_acosc(IN_ADDR_T ip, int32_t port, char *info, int32_t acosc_penalty_duration)
{
	return cs_check_v(ip, port, 1, info, acosc_penalty_duration);
}

void cs_add_violation(struct s_client *cl, char *info)
{
	struct s_module *module = get_module(cl);
	cs_add_violation_by_ip(cl->ip, module->ptab.ports[cl->port_idx].s_port, info);
}

void cs_add_auth_violation(struct s_client *cl, char *info)
{
	cs_add_violation_by_ip(cl->ip, 0, info);
}

void cs_add_violation_acosc(struct s_client *cl, char *info, int32_t acosc_penalty_duration)
{
	struct s_module *module = get_module(cl);
	cs_add_violation_by_ip_acosc(cl->ip, module->ptab.ports[cl->port_idx].s_port, info, acosc_penalty_duration);
}
