/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "opal_config.h"
#include "opal/constants.h"
#include "opal/types.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "opal/dss/dss.h"
#include "opal/mca/event/event.h"
#include "opal/mca/hwloc/base/base.h"
#include "opal/runtime/opal.h"
#include "opal/runtime/opal_progress_threads.h"
#include "opal/util/argv.h"
#include "opal/util/error.h"
#include "opal/util/output.h"
#include "opal/util/proc.h"
#include "opal/util/show_help.h"
#include "opal/mca/pmix/base/base.h"
#include "pmix120.h"

#include "pmix.h"
#include "pmix_server.h"

/****    S.O.U.T.H.B.O.U.N.D   I.N.T.E.R.F.A.C.E.S     ****/

/* These are the interfaces used by the OMPI/ORTE/OPAL layer to call
 * down into the embedded PMIx server. */

extern pmix_server_module_t pmix120_module;
extern opal_pmix_server_module_t *pmix120_host_module;
static char *dbgvalue=NULL;

int pmix120_server_init(opal_pmix_server_module_t *module,
                      opal_list_t *info)
{
    pmix_status_t rc;
    int dbg;
    opal_value_t *kv;
    pmix_info_t *pinfo;
    size_t sz, n;

    if (0 < (dbg = opal_output_get_verbosity(opal_pmix_base_framework.framework_output))) {
        asprintf(&dbgvalue, "PMIX_DEBUG=%d", dbg);
        putenv(dbgvalue);
    }

    /* convert the list to an array of pmix_info_t */
    if (NULL != info) {
        sz = opal_list_get_size(info);
        PMIX_INFO_CREATE(pinfo, sz);
        n = 0;
        OPAL_LIST_FOREACH(kv, info, opal_value_t) {
            (void)strncpy(pinfo[n].key, kv->key, PMIX_MAX_KEYLEN);
            pmix120_value_load(&pinfo[n].value, kv);
            ++n;
        }
    } else {
        sz = 0;
        pinfo = NULL;
    }

    if (PMIX_SUCCESS != (rc = PMIx_server_init(&pmix120_module, pinfo, sz))) {
        PMIX_INFO_FREE(pinfo, sz);
        return pmix120_convert_rc(rc);
    }
    PMIX_INFO_FREE(pinfo, sz);

    /* record the host module */
    pmix120_host_module = module;

    return OPAL_SUCCESS;
}

int pmix120_server_finalize(void)
{
    pmix_status_t rc;

    rc = PMIx_server_finalize();
    return pmix120_convert_rc(rc);
}

int pmix120_server_gen_regex(const char *input, char **regex)
{
    pmix_status_t rc;

    rc = PMIx_generate_regex(input, regex);
    return pmix120_convert_rc(rc);
}


int pmix120_server_gen_ppn(const char *input, char **ppn)
{
    pmix_status_t rc;

    rc = PMIx_generate_ppn(input, ppn);
    return pmix120_convert_rc(rc);
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    pmix120_opcaddy_t *op = (pmix120_opcaddy_t*)cbdata;

    if (NULL != op->opcbfunc) {
        op->opcbfunc(pmix120_convert_rc(status), op->cbdata);
    }
    OBJ_RELEASE(op);
}

int pmix120_server_register_nspace(opal_jobid_t jobid,
                                 int nlocalprocs,
                                 opal_list_t *info,
                                 opal_pmix_op_cbfunc_t cbfunc,
                                 void *cbdata)
{
    opal_value_t *kv, *k2;
    pmix_info_t *pinfo, *pmap;
    size_t sz, szmap, m, n;
    char nspace[PMIX_MAX_NSLEN];
    pmix_status_t rc;
    pmix120_opcaddy_t *op;
    opal_list_t *pmapinfo;
    opal_pmix120_jobid_trkr_t *job;

    /* convert the jobid */
    (void)opal_snprintf_jobid(nspace, PMIX_MAX_NSLEN, jobid);

    /* store this job in our list of known nspaces */
    job = OBJ_NEW(opal_pmix120_jobid_trkr_t);
    (void)strncpy(job->nspace, nspace, PMIX_MAX_NSLEN);
    job->jobid = jobid;
    opal_list_append(&mca_pmix_pmix120_component.jobids, &job->super);

    /* convert the list to an array of pmix_info_t */
    if (NULL != info) {
        sz = opal_list_get_size(info);
        PMIX_INFO_CREATE(pinfo, sz);
        n = 0;
        OPAL_LIST_FOREACH(kv, info, opal_value_t) {
            (void)strncpy(pinfo[n].key, kv->key, PMIX_MAX_KEYLEN);
            if (0 == strcmp(kv->key, OPAL_PMIX_PROC_DATA)) {
                pinfo[n].value.type = PMIX_INFO_ARRAY;
                /* the value contains a list of values - convert
                 * that list to another array */
                pmapinfo = (opal_list_t*)kv->data.ptr;
                szmap = opal_list_get_size(pmapinfo);
                PMIX_INFO_CREATE(pmap, szmap);
                pinfo[n].value.data.array.array = (struct pmix_info_t*)pmap;
                pinfo[n].value.data.array.size = szmap;
                m = 0;
                OPAL_LIST_FOREACH(k2, pmapinfo, opal_value_t) {
                    (void)strncpy(pmap[m].key, k2->key, PMIX_MAX_KEYLEN);
                    pmix120_value_load(&pmap[m].value, k2);
                    ++m;
                }
            } else {
                pmix120_value_load(&pinfo[n].value, kv);
            }
            ++n;
        }
    } else {
        sz = 0;
        pinfo = NULL;
    }

    /* setup the caddy */
    op = OBJ_NEW(pmix120_opcaddy_t);
    op->info = pinfo;
    op->sz = sz;
    op->opcbfunc = cbfunc;
    op->cbdata = cbdata;
    rc = PMIx_server_register_nspace(nspace, nlocalprocs, pinfo, sz,
                                     opcbfunc, op);
    if (PMIX_SUCCESS != rc) {
        OBJ_RELEASE(op);
    }
    return pmix120_convert_rc(rc);
}

void pmix120_server_deregister_nspace(opal_jobid_t jobid)
{
    opal_pmix120_jobid_trkr_t *jptr;

    /* if we don't already have it, we can ignore this */
    OPAL_LIST_FOREACH(jptr, &mca_pmix_pmix120_component.jobids, opal_pmix120_jobid_trkr_t) {
        if (jptr->jobid == jobid) {
            /* found it - tell the server to deregister */
            PMIx_server_deregister_nspace(jptr->nspace);
            /* now get rid of it from our list */
            opal_list_remove_item(&mca_pmix_pmix120_component.jobids, &jptr->super);
            OBJ_RELEASE(jptr);
            return;
        }
    }
}

int pmix120_server_register_client(const opal_process_name_t *proc,
                                 uid_t uid, gid_t gid,
                                 void *server_object,
                                 opal_pmix_op_cbfunc_t cbfunc,
                                 void *cbdata)
{
    pmix_status_t rc;
    pmix120_opcaddy_t *op;

    /* setup the caddy */
    op = OBJ_NEW(pmix120_opcaddy_t);
    op->opcbfunc = cbfunc;
    op->cbdata = cbdata;

    /* convert the jobid */
    (void)opal_snprintf_jobid(op->p.nspace, PMIX_MAX_NSLEN, proc->jobid);
    op->p.rank = proc->vpid;

    rc = PMIx_server_register_client(&op->p, uid, gid, server_object,
                                     opcbfunc, op);
    if (PMIX_SUCCESS != rc) {
        OBJ_RELEASE(op);
    }
    return pmix120_convert_rc(rc);
}

void pmix120_server_deregister_client(const opal_process_name_t *proc)
{
    opal_pmix120_jobid_trkr_t *jptr;
    pmix_proc_t p;

    /* if we don't already have it, we can ignore this */
    OPAL_LIST_FOREACH(jptr, &mca_pmix_pmix120_component.jobids, opal_pmix120_jobid_trkr_t) {
        if (jptr->jobid == proc->jobid) {
            /* found it - tell the server to deregister */
            (void)strncpy(p.nspace, jptr->nspace, PMIX_MAX_NSLEN);
            p.rank = proc->vpid;
            PMIx_server_deregister_client(&p);
            return;
        }
    }
}


int pmix120_server_setup_fork(const opal_process_name_t *proc, char ***env)
{
    pmix_status_t rc;
    pmix_proc_t p;

    /* convert the jobid */
    (void)opal_snprintf_jobid(p.nspace, PMIX_MAX_NSLEN, proc->jobid);
    p.rank = proc->vpid;

    rc = PMIx_server_setup_fork(&p, env);
    return pmix120_convert_rc(rc);
}

/* this is the call back up from the embedded PMIx server that
 * will contain the returned data. Note that the embedded server
 * "owns" the data and will free it upon return from this function */
static void dmdx_response(pmix_status_t status, char *data, size_t sz, void *cbdata)
{
    int rc;
    pmix120_opcaddy_t *op = (pmix120_opcaddy_t*)cbdata;

    rc = pmix120_convert_rc(status);
    if (NULL != op->mdxcbfunc) {
        op->mdxcbfunc(rc, data, sz, op->cbdata, NULL, NULL);
    }
    OBJ_RELEASE(op);
}

int pmix120_server_dmodex(const opal_process_name_t *proc,
                        opal_pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    pmix120_opcaddy_t *op;
    pmix_status_t rc;

    /* setup the caddy */
    op = OBJ_NEW(pmix120_opcaddy_t);
    op->mdxcbfunc = cbfunc;
    op->cbdata = cbdata;

    /* convert the jobid */
    (void)opal_snprintf_jobid(op->p.nspace, PMIX_MAX_NSLEN, proc->jobid);
    op->p.rank = proc->vpid;

    /* find the internally-cached data for this proc */
    rc = PMIx_server_dmodex_request(&op->p, dmdx_response, op);
    if (PMIX_SUCCESS != rc) {
        OBJ_RELEASE(op);
    }
    return pmix120_convert_rc(rc);
}

int pmix120_server_notify_error(int status,
                                opal_list_t *procs,
                                opal_list_t *error_procs,
                                opal_list_t *info,
                                opal_pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    opal_value_t *kv;
    pmix_info_t *pinfo;
    size_t sz, psz, esz, n;
    pmix_proc_t *ps, *eps;
    pmix_status_t rc;
    pmix120_opcaddy_t *op;
    opal_namelist_t *nm;

    /* convert the list of procs */
    if (NULL != procs) {
        psz = opal_list_get_size(procs);
        PMIX_PROC_CREATE(ps, psz);
        n = 0;
        OPAL_LIST_FOREACH(nm, procs, opal_namelist_t) {
            (void)opal_snprintf_jobid(ps[n].nspace, PMIX_MAX_NSLEN, nm->name.jobid);
            ps[n].rank = (int)nm->name.vpid;
            ++n;
        }
    } else {
        psz = 0;
        ps = NULL;
    }
    if (NULL != error_procs) {
        esz = opal_list_get_size(error_procs);
        PMIX_PROC_CREATE(eps, esz);
        n = 0;
        OPAL_LIST_FOREACH(nm, error_procs, opal_namelist_t) {
            (void)opal_snprintf_jobid(eps[n].nspace, PMIX_MAX_NSLEN, nm->name.jobid);
            eps[n].rank = (int)nm->name.vpid;
            ++n;
        }
    } else {
        esz = 0;
        eps = NULL;
    }

    /* convert the list to an array of pmix_info_t */
    if (NULL != info) {
        sz = opal_list_get_size(info);
        PMIX_INFO_CREATE(pinfo, sz);
        n = 0;
        OPAL_LIST_FOREACH(kv, info, opal_value_t) {
            (void)strncpy(pinfo[n].key, kv->key, PMIX_MAX_KEYLEN);
            pmix120_value_load(&pinfo[n].value, kv);
        }
    } else {
        sz = 0;
        pinfo = NULL;
    }

    /* setup the caddy */
    op = OBJ_NEW(pmix120_opcaddy_t);
    op->procs = ps;
    op->nprocs = psz;
    op->error_procs = eps;
    op->nerror_procs = esz;
    op->info = pinfo;
    op->sz = sz;
    op->opcbfunc = cbfunc;
    op->cbdata = cbdata;

    rc = pmix120_convert_opalrc(status);
    rc = PMIx_Notify_error(rc, ps, psz, eps, esz,
                                  pinfo, sz, opcbfunc, op);
    if (PMIX_SUCCESS != rc) {
        OBJ_RELEASE(op);
    }
    return pmix120_convert_rc(rc);
}
