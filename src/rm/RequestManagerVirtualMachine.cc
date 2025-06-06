/* -------------------------------------------------------------------------- */
/* Copyright 2002-2025, OpenNebula Project, OpenNebula Systems                */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0 (the "License"); you may    */
/* not use this file except in compliance with the License. You may obtain    */
/* a copy of the License at                                                   */
/*                                                                            */
/* http://www.apache.org/licenses/LICENSE-2.0                                 */
/*                                                                            */
/* Unless required by applicable law or agreed to in writing, software        */
/* distributed under the License is distributed on an "AS IS" BASIS,          */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   */
/* See the License for the specific language governing permissions and        */
/* limitations under the License.                                             */
/* -------------------------------------------------------------------------- */

#include "RequestManagerVirtualMachine.h"
#include "VirtualMachineDisk.h"
#include "PoolObjectAuth.h"
#include "Nebula.h"
#include "Quotas.h"
#include "ClusterPool.h"
#include "DatastorePool.h"
#include "HostPool.h"
#include "ImagePool.h"
#include "SecurityGroupPool.h"
#include "DispatchManager.h"
#include "VirtualMachineManager.h"

using namespace std;

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode RequestManagerVirtualMachine::vm_authorization_no_response(
        int                     oid,
        ImageTemplate *         tmpl,
        VirtualMachineTemplate* vtmpl,
        RequestAttributes&      att,
        PoolObjectAuth *        host_perm,
        PoolObjectAuth *        ds_perm,
        PoolObjectAuth *        img_perm)
{
    PoolObjectAuth vm_perms;

    if (auto object = pool->get_ro<PoolObjectSQL>(oid))
    {
        object->get_permissions(vm_perms);
    }
    else
    {
        att.resp_id = oid;

        return NO_EXISTS;
    }

    AuthRequest ar(att.uid, att.group_ids);

    ar.add_auth(att.auth_op, vm_perms);

    if (host_perm != nullptr)
    {
        ar.add_auth(AuthRequest::MANAGE, *host_perm);
    }

    if (tmpl != nullptr)
    {
        string t_xml;

        ar.add_create_auth(att.uid, att.gid, PoolObjectSQL::IMAGE,
                           tmpl->to_xml(t_xml));
    }

    if ( vtmpl != nullptr )
    {
        VirtualMachine::set_auth_request(att.uid, ar, vtmpl, true);
    }

    if ( ds_perm != nullptr )
    {
        ar.add_auth(AuthRequest::USE, *ds_perm);
    }

    if ( img_perm != nullptr )
    {
        ar.add_auth(AuthRequest::MANAGE, *img_perm);
    }

    if (UserPool::authorize(ar) == -1)
    {
        att.resp_msg = ar.message;

        return AUTHORIZATION;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

bool RequestManagerVirtualMachine::vm_authorization(
        int                     oid,
        ImageTemplate *         tmpl,
        VirtualMachineTemplate* vtmpl,
        RequestAttributes&      att,
        PoolObjectAuth *        host_perm,
        PoolObjectAuth *        ds_perm,
        PoolObjectAuth *        img_perm)
{
    auto ec = vm_authorization_no_response(oid, tmpl, vtmpl, att,
                                           host_perm, ds_perm, img_perm);
    if (ec != SUCCESS)
    {
        failure_response(ec, att);
        return false;
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

bool RequestManagerVirtualMachine::quota_resize_authorization(
        Template *          deltas,
        RequestAttributes&  att,
        PoolObjectAuth&     vm_perms)
{
    int rc;

    Nebula&    nd    = Nebula::instance();
    UserPool*  upool = nd.get_upool();
    GroupPool* gpool = nd.get_gpool();

    DefaultQuotas user_dquotas  = nd.get_default_user_quota();
    DefaultQuotas group_dquotas = nd.get_default_group_quota();

    if (vm_perms.uid != UserPool::ONEADMIN_ID)
    {
        if (auto user = upool->get(vm_perms.uid))
        {
            rc = user->quota.quota_update(Quotas::VM, deltas, user_dquotas, att.resp_msg);

            if (rc == false)
            {
                ostringstream oss;

                oss << object_name(PoolObjectSQL::USER) << " [" << vm_perms.uid << "] "
                    << att.resp_msg;

                att.resp_msg = oss.str();

                return false;
            }

            upool->update_quotas(user.get());
        }
    }

    if (vm_perms.gid != GroupPool::ONEADMIN_ID)
    {
        if ( auto group  = gpool->get(vm_perms.gid) )
        {
            rc = group->quota.quota_update(Quotas::VM, deltas, group_dquotas, att.resp_msg);

            if (rc == false)
            {
                ostringstream oss;
                RequestAttributes att_tmp(vm_perms.uid, -1, att);

                oss << object_name(PoolObjectSQL::GROUP) << " [" << vm_perms.gid << "] "
                    << att.resp_msg;

                att.resp_msg = oss.str();

                group.reset();

                quota_rollback(deltas, Quotas::VM, att_tmp);

                return false;
            }

            gpool->update_quotas(group.get());
        }
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode RequestManagerVirtualMachine::get_default_ds_information(
        int cluster_id,
        int& ds_id,
        string& tm_mad,
        RequestAttributes& att)
{
    Nebula& nd = Nebula::instance();

    ClusterPool* clpool = nd.get_clpool();

    bool ds_migr;

    set<int> ds_ids;

    ds_id = -1;

    if ( auto cluster = clpool->get_ro(cluster_id) )
    {
        ds_ids = cluster->get_datastore_ids();

        ds_id = Cluster::get_default_system_ds(ds_ids);
    }
    else
    {
        att.resp_obj = PoolObjectSQL::CLUSTER;
        att.resp_id  = cluster_id;

        return NO_EXISTS;
    }

    if (ds_id == -1)
    {
        ostringstream oss;

        oss << object_name(PoolObjectSQL::CLUSTER) << " [" << cluster_id
            << "] does not have any " << object_name(PoolObjectSQL::DATASTORE)
            << " of type " << Datastore::type_to_str(Datastore::SYSTEM_DS)
            << ".";

        att.resp_msg = oss.str();

        return ACTION;
    }

    set<int> ds_cluster_ids;

    return get_ds_information(ds_id, ds_cluster_ids, tm_mad, att, ds_migr);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode RequestManagerVirtualMachine::get_ds_information(int ds_id,
                                                     set<int>& ds_cluster_ids,
                                                     string& tm_mad,
                                                     RequestAttributes& att,
                                                     bool& ds_migr)
{
    Nebula& nd = Nebula::instance();

    auto ds = nd.get_dspool()->get_ro(ds_id);

    ds_cluster_ids.clear();

    if ( ds == nullptr )
    {
        att.resp_obj = PoolObjectSQL::DATASTORE;
        att.resp_id  = ds_id;

        return NO_EXISTS;
    }

    if ( ds->get_type() != Datastore::SYSTEM_DS )
    {
        ostringstream oss;

        oss << "Trying to use " << object_name(PoolObjectSQL::DATASTORE)
            << " [" << ds_id << "] to deploy the VM, but it is not of type"
            << " system datastore.";

        att.resp_msg = oss.str();

        return INTERNAL;
    }

    ds_cluster_ids = ds->get_cluster_ids();

    tm_mad = ds->get_tm_mad();

    ds->get_template_attribute("DS_MIGRATE", ds_migr);

    return SUCCESS;
}


/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

 Request::ErrorCode RequestManagerVirtualMachine::get_host_information(
        int     hid,
        string& name,
        string& vmm,
        int&    cluster_id,
        PoolObjectAuth&    host_perms,
        RequestAttributes& att)


{
    Nebula&    nd    = Nebula::instance();
    HostPool * hpool = nd.get_hpool();

    auto       host  = hpool->get_ro(hid);

    if ( host == nullptr )
    {
        att.resp_obj = PoolObjectSQL::HOST;
        att.resp_id  = hid;
        return NO_EXISTS;
    }

    if ( host->get_state() == Host::OFFLINE )
    {
        att.resp_msg = "Host is offline, cannot use it to deploy VM";

        return ACTION;
    }

    name = host->get_name();
    vmm  = host->get_vmm_mad();

    cluster_id = host->get_cluster_id();

    host->get_permissions(host_perms);

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

bool RequestManagerVirtualMachine::check_host(
        int hid, bool enforce, VirtualMachine* vm, string& error)
{
    HostPool * hpool = Nebula::instance().get_hpool();

    bool   test = true;
    string capacity_error;

    HostShareCapacity sr;

    vm->get_capacity(sr);

    auto host = hpool->get_ro(hid);

    if (host == nullptr)
    {
        error = "Host no longer exists";
        return false;
    }

    if ( enforce )
    {
        test = host->test_capacity(sr, capacity_error, enforce);
    }

    if (enforce && !test)
    {
        ostringstream oss;

        oss << object_name(PoolObjectSQL::VM) << " [" << vm->get_oid()
            << "] does not fit in " << object_name(PoolObjectSQL::HOST) << " ["
            << hid << "]. " << capacity_error;

        error = oss.str();
    }

    return test;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

unique_ptr<VirtualMachine> RequestManagerVirtualMachine::get_vm(int id,
        RequestAttributes& att)
{
    auto vm = pool->get<VirtualMachine>(id);

    if ( vm == nullptr )
    {
        att.resp_id = id;
        failure_response(NO_EXISTS, att);
        return nullptr;
    }

    return vm;
}

unique_ptr<VirtualMachine> RequestManagerVirtualMachine::get_vm_ro(int id,
        RequestAttributes& att)
{
    auto vm = pool->get_ro<VirtualMachine>(id);

    if ( vm == nullptr )
    {
        att.resp_id = id;
        failure_response(NO_EXISTS, att);
        return nullptr;
    }

    return vm;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int RequestManagerVirtualMachine::add_history(VirtualMachine * vm,
                                              int              hid,
                                              int              cid,
                                              const string&    hostname,
                                              const string&    vmm_mad,
                                              const string&    tm_mad,
                                              int              ds_id,
                                              RequestAttributes& att)
{
    VirtualMachinePool * vmpool = static_cast<VirtualMachinePool *>(pool);

    vm->add_history(hid, cid, hostname, vmm_mad, tm_mad, ds_id);

    if ( vmpool->insert_history(vm) != 0 )
    {
        att.resp_msg = "Cannot update virtual machine history";

        return -1;
    }

    if ( vmpool->update(vm) != 0 )
    {
        att.resp_msg = "Cannot update virtual machine";

        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineAction::request_execute(RequestAttributes& att,
                                                         const std::string& action_str,
                                                         int vid)
{
    int    rc;

    std::string memory, cpu;

    ostringstream oss;
    string error;

    VMActions::Action action;

    VirtualMachineTemplate quota_tmpl;
    RequestAttributes& att_aux(att);

    VMActions::action_from_str(action_str, action);

    // Update the authorization level for the action
    att.set_auth_op(action);

    auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, 0);

    if (auth != SUCCESS)
    {
        return auth;
    }

    auto vm = pool->get_ro<VirtualMachine>(vid);

    if (vm == nullptr)
    {
        att.resp_id = vid;
        return NO_EXISTS;
    }

    switch (action)
    {
        case VMActions::TERMINATE_ACTION:
            rc = dm->terminate(vid, false, att, error);
            break;
        case VMActions::TERMINATE_HARD_ACTION:
            rc = dm->terminate(vid, true, att, error);
            break;
        case VMActions::HOLD_ACTION:
            rc = dm->hold(vid, att, error);
            break;
        case VMActions::RELEASE_ACTION:
            rc = dm->release(vid, att, error);
            break;
        case VMActions::STOP_ACTION:
            rc = dm->stop(vid, att, error);
            break;
        case VMActions::SUSPEND_ACTION:
            rc = dm->suspend(vid, att, error);
            break;
        case VMActions::RESUME_ACTION:
            vm->get_quota_template(quota_tmpl, false, true);

            att_aux.uid = vm->get_uid();
            att_aux.gid = vm->get_gid();

            if (!quota_authorization(&quota_tmpl, Quotas::VIRTUALMACHINE, att_aux, att.resp_msg))
            {
                return ACTION;
            }

            rc = dm->resume(vid, att, error);

            if (rc < 0)
            {
                quota_rollback(&quota_tmpl, Quotas::VIRTUALMACHINE, att_aux);
            }

            break;
        case VMActions::REBOOT_ACTION:
            rc = dm->reboot(vid, false, att, error);
            break;
        case VMActions::REBOOT_HARD_ACTION:
            rc = dm->reboot(vid, true, att, error);
            break;
        case VMActions::RESCHED_ACTION:
            rc = dm->resched(vid, true, att, error);
            break;
        case VMActions::UNRESCHED_ACTION:
            rc = dm->resched(vid, false, att, error);
            break;
        case VMActions::POWEROFF_ACTION:
            rc = dm->poweroff(vid, false, att, error);
            break;
        case VMActions::POWEROFF_HARD_ACTION:
            rc = dm->poweroff(vid, true, att, error);
            break;
        case VMActions::UNDEPLOY_ACTION:
            rc = dm->undeploy(vid, false, att, error);
            break;
        case VMActions::UNDEPLOY_HARD_ACTION:
            rc = dm->undeploy(vid, true, att, error);
            break;
        default:
            rc = -3;
            break;
    }

    switch (rc)
    {
        case 0:
            return SUCCESS;

        case -1:
            att.resp_id = vid;
            return NO_EXISTS;

        case -2:
            oss << "Error performing action \"" << action_str << "\": " << error;

            att.resp_msg = oss.str();
            return ACTION;

        case -3:
            oss << "Action \"" << action_str << "\" is not supported";

            att.resp_msg = oss.str();
            return ACTION;

        default:
            att.resp_msg = "Internal error. Action result not defined";
            return INTERNAL;
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineAction::request_execute(xmlrpc_c::paramList const& paramList,
                                           RequestAttributes& att)
{
    string action_str = paramList.getString(1);
    int    vid        = paramList.getInt(2);

    auto ec = request_execute(att, action_str, vid);

    if ( ec == Request::SUCCESS)
    {
        success_response(vid, att);
    }
    else
    {
        failure_response(ec, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

/**
 *  Adds extra info to the volatile disks of the given template, ds inherited
 *  attributes and TYPE
 *    @param ds_id datastore id
 *    @param vd vector of DISKS
 *    @return true if there at least one volatile disk was found
 */
static bool set_volatile_disk_info(VirtualMachine *vm, int ds_id)
{
    VirtualMachineDisks& disks = vm->get_disks();

    bool found = disks.volatile_info(ds_id);

    if ( found )
    {
        Nebula::instance().get_vmpool()->update(vm);
    }

    return found;
}


static bool set_volatile_disk_info(VirtualMachine *vm, int ds_id, Template& tmpl)
{
    VirtualMachineDisks disks(&tmpl, false);

    bool found = disks.volatile_info(ds_id);

    if ( found )
    {
        Nebula::instance().get_vmpool()->update(vm);
    }

    return found;
}

/* -------------------------------------------------------------------------- */

int set_vnc_port(VirtualMachine *vm, int cluster_id, RequestAttributes& att)
{
    ClusterPool * cpool = Nebula::instance().get_clpool();

    VectorAttribute * graphics = vm->get_template_attribute("GRAPHICS");

    unsigned int port;
    int rc;

    if (graphics == nullptr)
    {
        return 0;
    }
    else if (vm->hasHistory() && vm->get_action()==VMActions::STOP_ACTION)
    {
        return 0;
    }
    else if (graphics->vector_value("PORT", port) == 0)
    {
        rc = cpool->set_vnc_port(cluster_id, port);

        if ( rc != 0 )
        {
            att.resp_msg = "Requested VNC port already assigned to a VM";
        }
    }
    else
    {
        rc = cpool->get_vnc_port(cluster_id, vm->get_oid(), port);

        if ( rc == 0 )
        {
            graphics->replace("PORT", port);

            Nebula::instance().get_vmpool()->update(vm);
        }
        else
        {
            att.resp_msg = "No free VNC ports available in the cluster";
        }
    }

    return rc;
}


static int set_migrate_vnc_port(VirtualMachine *vm, int cluster_id, bool keep)
{
    ClusterPool * cpool = Nebula::instance().get_clpool();

    VectorAttribute * graphics = vm->get_template_attribute("GRAPHICS");

    unsigned int previous_port;
    unsigned int port;

    int rc;

    // Do not update VM if no GRAPHICS or GRAPHICS/PORT defined
    if (graphics == nullptr)
    {
        return 0;
    }

    if (graphics->vector_value("PORT", previous_port) != 0)
    {
        return 0;
    }

    //live migrations need to keep VNC port
    if (keep)
    {
        rc = cpool->set_vnc_port(cluster_id, previous_port);

        port = previous_port;
    }
    else
    {
        rc = cpool->get_vnc_port(cluster_id, vm->get_oid(), port);
    }

    if ( rc != 0 )
    {
        return -1;
    }

    graphics->replace("PREVIOUS_PORT", previous_port);
    graphics->replace("PORT", port);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineDeploy::request_execute(RequestAttributes& att,
                                                         int vid,
                                                         int hid,
                                                         bool enforce,
                                                         int ds_id,
                                                         const string& str_tmpl)
{
    Nebula&             nd = Nebula::instance();
    DatastorePool * dspool = nd.get_dspool();

    VirtualMachineTemplate  tmpl;
    VirtualMachineTemplate  quota_tmpl, quota_tmpl_running;

    string hostname;
    string vmm_mad;
    int    cluster_id, old_cid = -1;
    int    uid;
    int    gid;

    PoolObjectAuth host_perms, ds_perms, vm_perms;
    PoolObjectAuth * auth_ds_perms;

    string tm_mad;

    bool check_nic_auto = false;

    if (!str_tmpl.empty())
    {
        check_nic_auto = !str_tmpl.empty();

        int rc = tmpl.parse_str_or_xml(str_tmpl, att.resp_msg);

        if ( rc != 0 )
        {
            return INTERNAL;
        }
    }

    auto ec = get_host_information(hid,
                                   hostname,
                                   vmm_mad,
                                   cluster_id,
                                   host_perms,
                                   att);
    if (ec != SUCCESS)
    {
        return ec;
    }

    // ------------------------------------------------------------------------
    // Get information about the system DS to use (tm_mad & permissions)
    // ------------------------------------------------------------------------
    if (auto vm = pool->get_ro<VirtualMachine>(vid))
    {
        if (vm->hasHistory() &&
            (vm->get_action() == VMActions::STOP_ACTION ||
             vm->get_action() == VMActions::UNDEPLOY_ACTION ||
             vm->get_action() == VMActions::UNDEPLOY_HARD_ACTION))
        {
            if (ds_id == -1)
            {
                ds_id = vm->get_ds_id();
            }

            check_nic_auto = false;
        }

        uid = vm->get_uid();
        gid = vm->get_gid();

        vm->get_permissions(vm_perms);

        enforce = enforce || vm->is_pinned();

        vm->get_quota_template(quota_tmpl_running, false, true);

        if (vm->hasHistory())
        {
            old_cid = vm->get_cid();
        }

        if (!vm->hasHistory() || (old_cid != -1 && (old_cid != cluster_id)))
        {
            vm->get_quota_template(quota_tmpl, true, false);
            vm->get_quota_template(quota_tmpl_running, false, true);
        }
    }
    else
    {
        att.resp_id = vid;

        return NO_EXISTS;
    }

    if ( ds_id == -1 ) //Use default system DS for cluster
    {
        ec = get_default_ds_information(cluster_id, ds_id, tm_mad, att);
        if (ec != SUCCESS)
        {
            return ec;
        }
    }
    else //Get information from user selected system DS
    {
        set<int> ds_cluster_ids;
        bool     ds_migr;

        ec = get_ds_information(ds_id, ds_cluster_ids, tm_mad, att, ds_migr);

        if (ec != SUCCESS)
        {
            return ec;
        }

        if (ds_cluster_ids.count(cluster_id) == 0)
        {
            ostringstream oss;

            oss << object_name(PoolObjectSQL::DATASTORE) << " [" << ds_id
                << "] and " << object_name(PoolObjectSQL::HOST) << " ["
                << hid << "] are not in the same "
                << object_name(PoolObjectSQL::CLUSTER) << " [" << cluster_id
                << "].";

            att.resp_msg = oss.str();

            return ACTION;
        }
    }

    if (ds_id == -1)
    {
        auth_ds_perms = 0;
    }
    else
    {
        auto ds = dspool->get_ro(ds_id);

        if (ds == nullptr )
        {
            att.resp_obj = PoolObjectSQL::DATASTORE;
            att.resp_id  = ds_id;

            return NO_EXISTS;
        }

        ds->get_permissions(ds_perms);

        auth_ds_perms = &ds_perms;
    }

    // ------------------------------------------------------------------------
    // Authorize request
    // ------------------------------------------------------------------------
    if ( check_nic_auto ) //Authorize network schedule and quotas
    {
        RequestAttributes att_quota(uid, gid, att);

        if (!att.is_admin())
        {
            string aname;

            if (tmpl.check_restricted(aname))
            {
                att.resp_msg = "NIC includes a restricted attribute " + aname;

                return AUTHORIZATION;
            }
        }

        if (!quota_authorization(&tmpl, Quotas::NETWORK, att_quota, att.resp_msg))
        {
            return AUTHORIZATION;
        }

        ec = vm_authorization_no_response(vid, 0, &tmpl, att, &host_perms, auth_ds_perms, 0);
    }
    else
    {
        ec = vm_authorization_no_response(vid, 0, 0, att, &host_perms, auth_ds_perms, 0);
    }

    if (ec != SUCCESS)
    {
        return ec;
    }

    // ------------------------------------------------------------------------
    // Check request consistency:
    // - VM States are right
    // - Host capacity if required
    // ------------------------------------------------------------------------
    auto vm = pool->get<VirtualMachine>(vid);

    if (vm == nullptr)
    {
        att.resp_id = vid;

        return NO_EXISTS;
    }

    if (vm->get_state() != VirtualMachine::PENDING &&
        vm->get_state() != VirtualMachine::HOLD &&
        vm->get_state() != VirtualMachine::STOPPED &&
        vm->get_state() != VirtualMachine::UNDEPLOYED)
    {
        att.resp_msg = "Deploy action is not available for state " +
                       vm->state_str();

        return ACTION;
    }

    if (check_host(hid, enforce || vm->is_pinned(), vm.get(), att.resp_msg) == false)
    {
        return ACTION;
    }

    if ( check_nic_auto && vm->get_auto_network_leases(&tmpl, att.resp_msg) != 0 )
    {
        return ACTION;
    }

    if ( vm->check_tm_mad_disks(tm_mad, att.resp_msg) != 0)
    {
        return ACTION;
    }

    if ( vm->check_shareable_disks(vmm_mad, att.resp_msg) != 0)
    {
        return ACTION;
    }

    if ( nd.get_vmm()->validate_template(vmm_mad, vm.get(), hid, cluster_id, att.resp_msg) != 0 )
    {
        return ACTION;
    }

    RequestAttributes att_quota(vm_perms.uid, vm_perms.gid, att);

    bool do_running_quota = vm->get_state() == VirtualMachine::STOPPED ||
                            vm->get_state() == VirtualMachine::UNDEPLOYED;

    // Authorize running quota (global and cluster)
    if (do_running_quota)
    {
        quota_tmpl_running.replace("CLUSTER_ID", cluster_id);

        if ( !quota_authorization(&quota_tmpl_running, Quotas::VM, att_quota, att_quota.resp_msg))
        {
            att.resp_msg = att_quota.resp_msg;

            return AUTHORIZATION;
        }
    }

    // Cluster quotas
    quota_tmpl.replace("CLUSTER_ID", cluster_id);
    quota_tmpl.add("SKIP_GLOBAL_QUOTA", true);
    if (old_cid == -1)
    {
        // Cluster quota on first deploy
        quota_tmpl.merge(&quota_tmpl_running);
        if ( !quota_authorization(&quota_tmpl, Quotas::VM, att_quota, att_quota.resp_msg))
        {
            att.resp_msg = att_quota.resp_msg;

            if (do_running_quota)
            {
                quota_rollback(&quota_tmpl_running, Quotas::VM, att_quota);
            }

            return AUTHORIZATION;
        }
    }
    else if (old_cid != cluster_id)
    {
        // Cluster quota, deploy on different cluster
        if ( !quota_authorization(&quota_tmpl, Quotas::VM, att_quota, att_quota.resp_msg))
        {
            att.resp_msg = att_quota.resp_msg;

            if (do_running_quota)
            {
                quota_rollback(&quota_tmpl_running, Quotas::VM, att_quota);
            }

            return AUTHORIZATION;
        }

        // Remove resources from old cluster
        quota_tmpl.replace("CLUSTER_ID", old_cid);
        Quotas::vm_del(vm_perms.uid, vm_perms.gid, &quota_tmpl);
    }

    // ------------------------------------------------------------------------
    // Add deployment dependent attributes to VM
    //   - volatile disk (selected system DS driver)
    //   - vnc port (free in the selected cluster)
    // ------------------------------------------------------------------------
    set_volatile_disk_info(vm.get(), ds_id);

    if (set_vnc_port(vm.get(), cluster_id, att) != 0)
    {
        if (do_running_quota)
        {
            quota_rollback(&quota_tmpl_running, Quotas::VM, att_quota);
        }

        if (old_cid == -1)
        {
            quota_rollback(&quota_tmpl, Quotas::VM, att_quota);
        }
        else if (old_cid != cluster_id)
        {
            // Remove resources back to old cluster
            quota_tmpl.replace("CLUSTER_ID", old_cid);
            Quotas::vm_add(vm_perms.uid, vm_perms.gid, &quota_tmpl);

            // Remove resources from new cluster
            quota_tmpl.replace("CLUSTER_ID", cluster_id);
            quota_rollback(&quota_tmpl, Quotas::VM, att_quota);
        }

        failure_response(ACTION, att);
        return ACTION;
    }

    // ------------------------------------------------------------------------
    // Add a new history record
    // ------------------------------------------------------------------------
    if (add_history(vm.get(),
                    hid,
                    cluster_id,
                    hostname,
                    vmm_mad,
                    tm_mad,
                    ds_id,
                    att) != 0)
    {
        return INTERNAL;
    }

    // ------------------------------------------------------------------------
    // deploy the VM (unlocks the vm object)
    // ------------------------------------------------------------------------

    dm->deploy(std::move(vm), att);

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDeploy::request_execute(xmlrpc_c::paramList const& paramList,
                                           RequestAttributes& att)
{
    // ------------------------------------------------------------------------
    // Get request parameters and information about the target host
    // ------------------------------------------------------------------------
    int  id      = xmlrpc_c::value_int(paramList.getInt(1));
    int  hid     = xmlrpc_c::value_int(paramList.getInt(2));
    bool enforce = false;
    int  ds_id   = -1;
    std::string str_tmpl;

    if ( paramList.size() > 3 )
    {
        enforce = xmlrpc_c::value_boolean(paramList.getBoolean(3));
    }

    if ( paramList.size() > 4 )
    {
        ds_id = xmlrpc_c::value_int(paramList.getInt(4));
    }

    if ( paramList.size() > 5 ) // Template with network scheduling results
    {
        str_tmpl = xmlrpc_c::value_string(paramList.getString(5));
    }

    auto ec = request_execute(att, id, hid, enforce, ds_id, str_tmpl);

    if (ec != SUCCESS)
    {
        failure_response(ec, att);

        return;
    }

    success_response(id, att);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineMigrate::request_execute(RequestAttributes& att,
                                                          int vid,
                                                          int hid,
                                                          bool live,
                                                          bool enforce,
                                                          int ds_id,
                                                          int poweroff)
{
    Nebula& nd = Nebula::instance();

    DatastorePool * dspool = nd.get_dspool();

    string hostname;
    string vmm_mad;
    int    cluster_id;
    set<int> ds_cluster_ids;
    PoolObjectAuth host_perms, ds_perms, vm_perms;
    PoolObjectAuth * auth_ds_perms;
    VirtualMachineTemplate quota_tmpl;

    int    c_hid;
    int    c_cluster_id;
    int    c_ds_id;
    string c_tm_mad, tm_mad;

    set<int> cluster_ids;
    string   error_str;

    bool ds_migr;

    auto ec = get_host_information(hid,
                                   hostname,
                                   vmm_mad,
                                   cluster_id,
                                   host_perms,
                                   att);

    if (ec != SUCCESS)
    {
        return ec;
    }

    if (ds_id == -1)
    {
        auth_ds_perms = 0;
    }
    else
    {
        auto ds = dspool->get_ro(ds_id);

        if (ds == nullptr )
        {
            att.resp_obj = PoolObjectSQL::DATASTORE;
            att.resp_id  = ds_id;

            return NO_EXISTS;
        }

        ds->get_permissions(ds_perms);

        auth_ds_perms = &ds_perms;
    }

    // ------------------------------------------------------------------------
    // Authorize request
    // ------------------------------------------------------------------------
    ec = vm_authorization_no_response(vid, 0, 0, att, &host_perms, auth_ds_perms, 0);

    if (ec != SUCCESS)
    {
        return ec;
    }

    // ------------------------------------------------------------------------
    // Check request consistency:
    // - VM States are right and there is at least a history record
    // - New host is not the current one
    // - Host capacity if required
    // - Compatibility with PCI devices
    // - New host and current one are in the same cluster
    // ------------------------------------------------------------------------
    auto vm = pool->get<VirtualMachine>(vid);

    if (vm == nullptr)
    {
        att.resp_id = vid;

        return NO_EXISTS;
    }

    if (vm->is_previous_history_open() ||
        (vm->get_state() != VirtualMachine::POWEROFF &&
         vm->get_state() != VirtualMachine::SUSPENDED &&
         (vm->get_state() != VirtualMachine::ACTIVE ||
          (vm->get_lcm_state() != VirtualMachine::RUNNING &&
           vm->get_lcm_state() != VirtualMachine::UNKNOWN))))
    {
        att.resp_msg = "Migrate action is not available for state " + vm->state_str();

        return ACTION;
    }

    if (live && vm->is_pinned())
    {
        att.resp_msg = "VM with a pinned NUMA topology cannot be live-migrated";

        return ACTION;
    }

    // Get System DS information from current History record
    c_ds_id  = vm->get_ds_id();
    c_tm_mad = vm->get_tm_mad();

    // Check we are not migrating to the same host and the same system DS
    c_hid = vm->get_hid();

    if (c_hid == hid && (ds_id == -1 || ds_id == c_ds_id))
    {
        ostringstream oss;

        oss << "VM is already running on host [" << c_hid << "] and datastore [" << c_ds_id << "]";

        att.resp_msg = oss.str();

        return ACTION;
    }

    // Check the host has enough capacity
    if (check_host(hid, enforce, vm.get(), att.resp_msg) == false)
    {
        return ACTION;
    }

    int rc = vm->automatic_requirements(cluster_ids, error_str);

    if (rc != 0)
    {
        att.resp_msg = error_str;

        return ACTION;
    }

    //Check PCI devices are compatible with migration type
    HostShareCapacity sr;

    vm->get_capacity(sr);

    if ((sr.pci.size() > 0) && (!poweroff &&
                                vm->get_state() != VirtualMachine::POWEROFF))
    {
        ostringstream oss;

        oss << "Cannot migrate VM [" << vid << "], use poweroff or poweroff-hard"
            " flag for migrating a VM with PCI devices";

        att.resp_msg = oss.str();

        return ACTION;
    }

    // Check we are migrating to a compatible cluster
    if (auto host = nd.get_hpool()->get_ro(c_hid))
    {
        c_cluster_id = host->get_cluster_id();
    }
    else
    {
        att.resp_obj = PoolObjectSQL::HOST;
        att.resp_id  = c_hid;

        return NO_EXISTS;
    }

    if (!cluster_ids.empty() && cluster_ids.count(cluster_id) == 0)
    {
        ostringstream oss;

        oss << "Cannot migrate  VM [" << vid << "] to host [" << hid << "]. Host is in cluster ["
            << cluster_id << "], and VM requires to be placed on cluster ["
            << one_util::join(cluster_ids, ',') << "]";

        att.resp_msg = oss.str();

        return ACTION;
    }

    if (ds_id != -1 && c_ds_id != ds_id)
    {
        VirtualMachineManager * vmm = Nebula::instance().get_vmm();

        const VirtualMachineManagerDriver * vmmd = vmm->get(vmm_mad);

        if ( vmmd == nullptr )
        {
            att.resp_msg = "Cannot find vmm driver: " + vmm_mad;

            return ACTION;
        }

        if (live && !vmmd->is_ds_live_migration())
        {
            att.resp_msg = "A migration to a different system datastore "
                           "cannot be performed live.";

            return ACTION;
        }

        ec = get_ds_information(ds_id, ds_cluster_ids, tm_mad, att, ds_migr);

        if (ec != SUCCESS)
        {
            return ec;
        }

        if (!ds_migr)
        {
            att.resp_msg = "System datastore migration not supported by driver";

            return ACTION;
        }

        if (c_tm_mad != tm_mad)
        {
            att.resp_msg = "Cannot migrate to a system datastore with a different driver";

            return ACTION;
        }
    }
    else
    {
        ds_id  = c_ds_id;

        ec = get_ds_information(ds_id, ds_cluster_ids, tm_mad, att, ds_migr);

        if (ec != SUCCESS)
        {
            return ec;
        }
    }

    if (!ds_cluster_ids.empty() && ds_cluster_ids.count(cluster_id) == 0)
    {
        ostringstream oss;

        oss << "Cannot migrate VM [" << vid << "] to host [" << hid
            << "] and system datastore [" << ds_id << "]. Host is in cluster ["
            << cluster_id << "], and the datastore is in cluster ["
            << one_util::join(ds_cluster_ids, ',') << "]";

        att.resp_msg = oss.str();

        return ACTION;
    }

    // -------------------------------------------------------------------------
    // Request a new VNC port in the new cluster
    // -------------------------------------------------------------------------
    vm->get_permissions(vm_perms);
    RequestAttributes att_quota(vm_perms.uid, vm_perms.gid, att);

    if ( c_cluster_id != cluster_id )
    {
        if ( set_migrate_vnc_port(vm.get(), cluster_id, live) == -1 )
        {
            att.resp_msg = "No free VNC port available in the new cluster";

            return ACTION;
        }

        // Check cluster quotas on new cluster, remove resources from old cluster
        vm->get_quota_template(quota_tmpl, true, vm->is_running_quota());

        quota_tmpl.replace("CLUSTER_ID", cluster_id);
        quota_tmpl.add("SKIP_GLOBAL_QUOTA", true);

        if ( !quota_authorization(&quota_tmpl, Quotas::VM, att_quota, att_quota.resp_msg))
        {
            att.resp_msg = att_quota.resp_msg;
            return AUTHORIZATION;
        }

        quota_tmpl.replace("CLUSTER_ID", c_cluster_id);
        quota_rollback(&quota_tmpl, Quotas::VM, att_quota);
    }

    // ------------------------------------------------------------------------
    // Add a new history record and update volatile DISK attributes
    // ------------------------------------------------------------------------

    set_volatile_disk_info(vm.get(), ds_id);

    //add_history call will also update the vm
    if (add_history(vm.get(),
                    hid,
                    cluster_id,
                    hostname,
                    vmm_mad,
                    tm_mad,
                    ds_id,
                    att) != 0)
    {
        vm.reset();

        // quota rollback
        if (c_cluster_id != cluster_id)
        {
            Quotas::vm_add(vm_perms.uid, vm_perms.gid, &quota_tmpl);

            Quotas::vm_del(vm_perms.uid, vm_perms.gid, &quota_tmpl);
        }

        return INTERNAL;
    }

    // ------------------------------------------------------------------------
    // Migrate the VM
    // ------------------------------------------------------------------------
    if (live && vm->get_lcm_state() == VirtualMachine::RUNNING )
    {
        rc = dm->live_migrate(vm.get(), att);
    }
    else
    {
        rc = dm->migrate(vm.get(), poweroff, att);
    }

    if (rc != 0)
    {
        vm.reset();

        // Cluster quota rollback
        if (c_cluster_id != cluster_id)
        {
            Quotas::vm_add(vm_perms.uid, vm_perms.gid, &quota_tmpl);

            Quotas::vm_del(vm_perms.uid, vm_perms.gid, &quota_tmpl);
        }

        return INTERNAL;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineMigrate::request_execute(xmlrpc_c::paramList const& paramList,
                                            RequestAttributes& att)
{
    // ------------------------------------------------------------------------
    // Get request parameters and information about the target host
    // ------------------------------------------------------------------------

    int  id      = xmlrpc_c::value_int(paramList.getInt(1));
    int  hid     = xmlrpc_c::value_int(paramList.getInt(2));
    bool live    = xmlrpc_c::value_boolean(paramList.getBoolean(3));
    bool enforce = false;
    int  ds_id   = -1;
    int  poffmgr = 0;

    if ( paramList.size() > 4 )
    {
        enforce = xmlrpc_c::value_boolean(paramList.getBoolean(4));
    }

    if ( paramList.size() > 5 )
    {
        ds_id = xmlrpc_c::value_int(paramList.getInt(5));
    }

    if ( paramList.size() > 6 )
    {
        poffmgr = xmlrpc_c::value_int(paramList.getInt(6));
    }

    auto ec = request_execute(att, id, hid, live, enforce, ds_id, poffmgr);

    if (ec != SUCCESS)
    {
        failure_response(ec, att);

        return;
    }

    success_response(id, att);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDiskSaveas::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    Nebula& nd = Nebula::instance();

    ImagePool *     ipool  = nd.get_ipool();
    DatastorePool * dspool = nd.get_dspool();

    int    id       = xmlrpc_c::value_int(paramList.getInt(1));
    int    disk_id  = xmlrpc_c::value_int(paramList.getInt(2));
    string img_name = xmlrpc_c::value_string(paramList.getString(3));
    string img_type = xmlrpc_c::value_string(paramList.getString(4));
    int    snap_id  = xmlrpc_c::value_int(paramList.getInt(5));

    VirtualMachinePool * vmpool = static_cast<VirtualMachinePool *>(pool);

    int iid;
    int iid_orig;

    string         ds_data;
    PoolObjectAuth ds_perms;
    long long      avail;
    bool           ds_check;

    string ds_mad;
    string tm_mad;

    string target;
    string dev_prefix;

    int       ds_id;
    string    ds_name;
    long long size;

    string iname_orig;
    string iuname_orig;

    Image::ImageType type;
    Image::DiskType  ds_disk_type;

    unique_ptr<ImageTemplate> itemplate;
    Template        img_usage;

    int    rc;
    bool   rc_auth;

    // -------------------------------------------------------------------------
    // Prepare and check the VM/DISK to be saved as
    // -------------------------------------------------------------------------
    if ( auto vm = get_vm(id, att) )
    {
        if (vm->set_saveas_state() != 0)
        {
            att.resp_msg = "VM has to be RUNNING, POWEROFF or SUSPENDED to save disks.";
            failure_response(INTERNAL, att);
            return;
        }

        rc = vm->set_saveas_disk(disk_id, snap_id, iid_orig, size, att.resp_msg);

        if (rc == -1)
        {
            vm->clear_saveas_state();
            vm->clear_saveas_disk();

            att.resp_msg ="Cannot use DISK. " + att.resp_msg;
            failure_response(INTERNAL, att);
            return;
        }

        vmpool->update(vm.get());
    }
    else
    {
        return;
    }

    // -------------------------------------------------------------------------
    // Get the data of the Image to be saved
    // -------------------------------------------------------------------------
    if ( auto img = ipool->get_ro(iid_orig) )
    {
        ds_id   = img->get_ds_id();
        ds_name = img->get_ds_name();

        type = img->get_type();

        iname_orig  = img->get_name();
        iuname_orig = img->get_uname();

        img->get_template_attribute("TARGET", target);
        img->get_template_attribute("DEV_PREFIX", dev_prefix);
    }
    else
    {
        goto error_image;
    }

    switch (type)
    {
        case Image::OS:
        case Image::DATABLOCK:
        case Image::CDROM:
            break;

        case Image::KERNEL:
        case Image::RAMDISK:
        case Image::CONTEXT:
        case Image::BACKUP:
            goto error_image_type;
    }

    // -------------------------------------------------------------------------
    // Get the data of the DataStore for the new image & size
    // -------------------------------------------------------------------------
    if ( auto ds = dspool->get_ro(ds_id) )
    {
        ds->get_permissions(ds_perms);
        ds->to_xml(ds_data);

        ds_check     = ds->get_avail_mb(avail);
        ds_disk_type = ds->get_disk_type();
        ds_mad       = ds->get_ds_mad();
        tm_mad       = ds->get_tm_mad();
    }
    else
    {
        goto error_ds;
    }


    if (ds_check && (size > avail))
    {
        goto error_size;
    }

    // -------------------------------------------------------------------------
    // Create a template for the new Image
    // -------------------------------------------------------------------------
    itemplate = make_unique<ImageTemplate>();

    itemplate->add("NAME", img_name);
    itemplate->add("SIZE", size);

    itemplate->add("SAVED_IMAGE_ID", iid_orig);
    itemplate->add("SAVED_DISK_ID", disk_id);
    itemplate->add("SAVED_VM_ID", id);
    itemplate->set_saving();

    if (img_type.empty())
    {
        itemplate->add("TYPE", Image::type_to_str(type));
    }
    else
    {
        itemplate->add("TYPE", img_type);
    }

    if (!target.empty())
    {
        itemplate->add("TARGET", target);
    }

    if (!dev_prefix.empty())
    {
        itemplate->add("DEV_PREFIX", dev_prefix);
    }

    Image::test_set_persistent(itemplate.get(), att.uid, att.gid, false);

    img_usage.add("SIZE",      size);
    img_usage.add("DATASTORE", ds_id);

    // -------------------------------------------------------------------------
    // Authorize the operation & check quotas
    // -------------------------------------------------------------------------
    rc_auth = vm_authorization(id, itemplate.get(), 0, att, 0, &ds_perms, 0);

    if ( rc_auth == true )
    {
        rc_auth = quota_authorization(&img_usage, Quotas::DATASTORE, att);
    }

    if ( rc_auth == false)
    {
        goto error_auth;
    }

    // -------------------------------------------------------------------------
    // Create the image
    // -------------------------------------------------------------------------
    rc = ipool->allocate(att.uid,
                         att.gid,
                         att.uname,
                         att.gname,
                         att.umask,
                         std::move(itemplate),
                         ds_id,
                         ds_name,
                         ds_disk_type,
                         ds_data,
                         Datastore::IMAGE_DS,
                         ds_mad,
                         tm_mad,
                         "",
                         -1,
                         &iid,
                         att.resp_msg);
    if (rc < 0)
    {
        goto error_allocate;
    }

    if ( auto ds = dspool->get(ds_id) )
    {
        ds->add_image(iid);

        dspool->update(ds.get());
    }
    else
    {
        goto error_ds_removed;
    }

    success_response(iid, att);

    return;

error_image:
    att.resp_obj = PoolObjectSQL::IMAGE;
    att.resp_id  = iid_orig;
    failure_response(NO_EXISTS, att);
    goto error_common;

error_image_type:
    att.resp_msg = "Cannot save_as image of type " + Image::type_to_str(type);
    failure_response(INTERNAL, att);
    goto error_common;

error_ds:
error_ds_removed:
    att.resp_obj = PoolObjectSQL::DATASTORE;
    att.resp_id  = ds_id;
    failure_response(NO_EXISTS, att);
    goto error_common;

error_size:
    att.resp_msg = "Not enough space in datastore";
    failure_response(ACTION, att);
    goto error_common;

error_auth:
    goto error_common;

error_allocate:
    att.resp_obj = PoolObjectSQL::IMAGE;
    quota_rollback(&img_usage, Quotas::DATASTORE, att);
    failure_response(ALLOCATE, att);
    goto error_common;

error_common:
    if (auto vm = vmpool->get(id))
    {
        vm->clear_saveas_state();

        vm->clear_saveas_disk();

        vmpool->update(vm.get());
    }

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineMonitoring::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int  id = xmlrpc_c::value_int(paramList.getInt(1));
    int  rc;

    string oss;

    bool auth = vm_authorization(id, 0, 0, att, 0, 0, 0);

    if ( auth == false )
    {
        return;
    }

    rc = (static_cast<VirtualMachinePool *>(pool))->dump_monitoring(oss, id);

    if ( rc != 0 )
    {
        att.resp_msg = "Internal error";
        failure_response(INTERNAL, att);
        return;
    }

    success_response(oss, att);

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineAttach::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    VirtualMachineTemplate  tmpl;

    int     rc;

    int     id       = xmlrpc_c::value_int(paramList.getInt(1));
    string  str_tmpl = xmlrpc_c::value_string(paramList.getString(2));

    // -------------------------------------------------------------------------
    // Check if the VM is a Virtual Router
    // -------------------------------------------------------------------------
    if ( auto vm = pool->get_ro<VirtualMachine>(id) )
    {
        if ( !vm->hasHistory() )
        {
            att.resp_msg = "VM is not running in any host";
            failure_response(ACTION, att);

            return;
        }

        if (vm->is_vrouter())
        {
            att.resp_msg = "Action is not supported for virtual router VMs";
            failure_response(Request::ACTION, att);

            return;
        }
    }
    else
    {
        att.resp_id = id;
        att.resp_obj = PoolObjectSQL::VM;

        failure_response(NO_EXISTS, att);

        return;
    }

    // -------------------------------------------------------------------------
    // Parse disk template
    // -------------------------------------------------------------------------
    rc = tmpl.parse_str_or_xml(str_tmpl, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(INTERNAL, att);
        return;
    }

    // -------------------------------------------------------------------------
    // Perform the attach
    // -------------------------------------------------------------------------
    ErrorCode ec = request_execute(id, tmpl, att);

    if ( ec == SUCCESS )
    {
        success_response(id, att);
    }
    else
    {
        failure_response(ec, att);
    }

}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineAttach::request_execute(int id,
                                                         VirtualMachineTemplate& tmpl, RequestAttributes& att)
{
    Nebula&           nd = Nebula::instance();
    VirtualMachinePool * vmpool = nd.get_vmpool();

    PoolObjectAuth         vm_perms;

    bool   volatile_disk;

    VirtualMachineTemplate deltas(tmpl);

    // -------------------------------------------------------------------------
    // Authorize the operation & check quotas
    // -------------------------------------------------------------------------
    if (auto vm = vmpool->get_ro(id))
    {
        vm->get_permissions(vm_perms);

        volatile_disk = set_volatile_disk_info(vm.get(), vm->get_ds_id(), tmpl);

        if (vm->hasHistory())
        {
            deltas.add("CLUSTER_ID", vm->get_cid());
        }
    }
    else
    {
        att.resp_id  = id;
        att.resp_obj = PoolObjectSQL::VM;
        return NO_EXISTS;
    }

    AuthRequest ar(att.uid, att.group_ids);

    ar.add_auth(att.auth_op, vm_perms);

    VirtualMachine::set_auth_request(att.uid, ar, &tmpl, true);

    if (UserPool::authorize(ar) == -1)
    {
        att.resp_msg = ar.message;
        return AUTHORIZATION;
    }

    if (!att.is_admin())
    {
        string aname;

        if (tmpl.check_restricted(aname))
        {
            att.resp_msg = "DISK includes a restricted attribute " + aname;
            return AUTHORIZATION;
        }
    }

    RequestAttributes att_quota(vm_perms.uid, vm_perms.gid, att);

    VirtualMachineDisks::extended_info(att.uid, &deltas);

    if (quota_resize_authorization(&deltas, att_quota, vm_perms) == false)
    {
        att.resp_msg = std::move(att_quota.resp_msg);
        return AUTHORIZATION;
    }

    if (!volatile_disk)
    {
        if ( quota_authorization(&tmpl, Quotas::IMAGE, att_quota, att.resp_msg) == false )
        {
            quota_rollback(&deltas, Quotas::VM, att_quota);
            return AUTHORIZATION;
        }
    }

    // Datastore quotas
    vector<unique_ptr<Template>> ds_quotas;

    VirtualMachineDisks::image_ds_quotas(&deltas, ds_quotas);

    if ( !ds_quotas.empty() &&
         !quota_authorization(ds_quotas[0].get(), Quotas::DATASTORE, att_quota, att.resp_msg))
    {
        quota_rollback(&deltas, Quotas::VM, att_quota);

        if (!volatile_disk)
        {
            quota_rollback(&tmpl, Quotas::IMAGE, att_quota);
        }

        return AUTHORIZATION;
    }

    if ( dm->attach(id, &tmpl, att, att.resp_msg) != 0 )
    {
        quota_rollback(&deltas, Quotas::VM, att_quota);

        if (!volatile_disk)
        {
            quota_rollback(&tmpl, Quotas::IMAGE, att_quota);
        }

        if (!ds_quotas.empty())
        {
            quota_rollback(ds_quotas[0].get(), Quotas::DATASTORE, att_quota);
        }

        return ACTION;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDetach::request_execute(xmlrpc_c::paramList const& paramList,
                                           RequestAttributes& att)
{
    int rc;

    int     id      = xmlrpc_c::value_int(paramList.getInt(1));
    int     disk_id = xmlrpc_c::value_int(paramList.getInt(2));

    // -------------------------------------------------------------------------
    // Authorize the operation
    // -------------------------------------------------------------------------
    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    if ( auto vm = get_vm(id, att) )
    {
        if (vm->is_vrouter())
        {
            att.resp_msg = "Action is not supported for virtual router VMs";
            failure_response(ACTION, att);

            return;
        }
    }
    else
    {
        return;
    }


    rc = dm->detach(id, disk_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(ACTION, att);
    }
    else
    {
        success_response(id, att);
    }

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineResize::request_execute(xmlrpc_c::paramList const& paramList,
                                           RequestAttributes& att)
{
    int id = xmlrpc_c::value_int(paramList.getInt(1));
    std::string str_tmpl = xmlrpc_c::value_string(paramList.getString(2));

    bool enforce = !att.is_admin();

    if ( paramList.size() > 2 && att.is_admin())
    {
        enforce = paramList.getBoolean(3);
    }

    float ncpu, ocpu, dcpu;
    long  nmemory, omemory, dmemory;
    int   nvcpu, ovcpu, dvcpu;
    bool  update_running_quota;

    Template deltas;

    PoolObjectAuth vm_perms;

    VirtualMachinePool * vmpool = static_cast<VirtualMachinePool *>(pool);
    VirtualMachineTemplate tmpl;

    // -------------------------------------------------------------------------
    // Parse template
    // -------------------------------------------------------------------------
    if ( tmpl.parse_str_or_xml(str_tmpl, att.resp_msg) != 0 )
    {
        failure_response(INTERNAL, att);
        return;
    }

    /* ---------------------------------------------------------------------- */
    /*  Authorize the operation & restricted attributes                       */
    /* ---------------------------------------------------------------------- */
    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    if (!att.is_admin())
    {
        string aname;

        if (tmpl.check_restricted(aname))
        {
            att.resp_msg = "Template includes a restricted attribute " + aname;
            failure_response(AUTHORIZATION, att);

            return;
        }
    }

    /* ---------------------------------------------------------------------- */
    /*  Get the resize values                                                 */
    /* ---------------------------------------------------------------------- */

    tmpl.get("CPU", ncpu);
    tmpl.get("VCPU", nvcpu);
    tmpl.get("MEMORY", nmemory);

    if (ncpu < 0)
    {
        att.resp_msg = "CPU must be a positive float or integer value.";

        failure_response(INTERNAL, att);
        return;
    }

    if (nmemory < 0)
    {
        att.resp_msg = "MEMORY must be a positive integer value.";

        failure_response(INTERNAL, att);
        return;
    }

    if (nvcpu < 0)
    {
        att.resp_msg = "VCPU must be a positive integer value.";

        failure_response(INTERNAL, att);
        return;
    }

    /* ---------------------------------------------------------------------- */
    /*  Compute deltas and check quotas                                       */
    /* ---------------------------------------------------------------------- */
    if ( auto vm = vmpool->get_ro(id) )
    {
        vm->get_permissions(vm_perms);

        vm->get_template_attribute("MEMORY", omemory);
        vm->get_template_attribute("CPU", ocpu);
        vm->get_template_attribute("VCPU", ovcpu);

        if (vm->is_pinned())
        {
            ncpu = nvcpu;
        }

        auto state = vm->get_state();

        if (vm->hasHistory())
        {
            deltas.add("CLUSTER_ID", vm->get_cid());
        }

        update_running_quota = state == VirtualMachine::PENDING ||
                               state == VirtualMachine::HOLD || (state == VirtualMachine::ACTIVE &&
                                                                 vm->get_lcm_state() == VirtualMachine::RUNNING);
    }
    else
    {
        att.resp_id = id;
        failure_response(NO_EXISTS, att);
        return;
    }

    if (nmemory == 0)
    {
        nmemory = omemory;
    }

    if (ncpu == 0)
    {
        ncpu = ocpu;
    }

    if (nvcpu == 0)
    {
        nvcpu = ovcpu;
    }

    dcpu    = ncpu - ocpu;
    dvcpu   = nvcpu - ovcpu;
    dmemory = nmemory - omemory;

    deltas.add("MEMORY", dmemory);
    deltas.add("CPU", dcpu);
    deltas.add("VCPU", dvcpu);

    if (update_running_quota)
    {
        deltas.add("RUNNING_MEMORY", dmemory);
        deltas.add("RUNNING_CPU", dcpu);
        deltas.add("RUNNING_VCPU", dvcpu);
    }

    if (quota_resize_authorization(&deltas, att, vm_perms) == false)
    {
        failure_response(AUTHORIZATION, att);
        return;
    }

    RequestAttributes att_rollback(vm_perms.uid, vm_perms.gid, att);

    if ( dm->resize(id, ncpu, nvcpu, nmemory, enforce, att, att.resp_msg) == -1 )
    {
        quota_rollback(&deltas, Quotas::VM, att_rollback);

        failure_response(ACTION, att);

        return;
    }

    success_response(id, att);

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineSnapshotCreate::request_execute(RequestAttributes& att,
        int vid,
        string& name)
{
    PoolObjectAuth   vm_perms;

    int     rc;
    int     snap_id;
    int     cid = -1;

    VectorAttribute* snap = nullptr;

    // -------------------------------------------------------------------------
    // Authorize the operation
    // -------------------------------------------------------------------------
    auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, 0);

    if (auth != SUCCESS)
    {
        return auth;
    }

    // Check if the action is supported for imported VMs
    if (auto vm = pool->get_ro<VirtualMachine>(vid))
    {
        auto vm_bck = vm->backups();

        if ( vm_bck.configured() && vm_bck.mode() == Backups::INCREMENT )
        {
            att.resp_msg = "Action \"snapshot-create\" is not compatible with "
                           "incremental backups";

            return ACTION;
        }

        // get quota deltas
        snap = vm->new_snapshot(name, snap_id);
        snap = snap->clone();

        vm->get_permissions(vm_perms);

        if (vm->hasHistory())
        {
            cid = vm->get_cid();
        }
    }
    else
    {
        att.resp_id = vid;
        return NO_EXISTS;
    }

    Template quota_tmpl;

    quota_tmpl.add("CLUSTER_ID", cid);
    quota_tmpl.set(snap);

    RequestAttributes att_quota(vm_perms.uid, vm_perms.gid, att);

    if ( !quota_authorization(&quota_tmpl, Quotas::VM, att_quota, att.resp_msg) )
    {
        // todo Double check the return code, should we copy vm_perms.uid and vm_perms.gid to response
        return AUTHORIZATION;
    }

    rc = dm->snapshot_create(vid, name, snap_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        quota_rollback(&quota_tmpl, Quotas::VM, att_quota);
        return ACTION;
    }

    att.resp_id = snap_id;
    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineSnapshotCreate::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int     vid  = paramList.getInt(1);
    string  name = paramList.getString(2);

    auto rc = request_execute(att, vid, name);

    if (rc == Request::SUCCESS)
    {
        success_response(att.resp_id, att);
    }
    else
    {
        failure_response(rc, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineSnapshotRevert::request_execute(
        RequestAttributes& att,
        int vid,
        int snap_id)
{
    int rc;

    // -------------------------------------------------------------------------
    // Authorize the operation
    // -------------------------------------------------------------------------
    auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, 0);

    if (auth != SUCCESS)
    {
        return auth;
    }

    rc = dm->snapshot_revert(vid, snap_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        return ACTION;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineSnapshotRevert::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int vid     = paramList.getInt(1);
    int snap_id = paramList.getInt(2);

    auto rc = request_execute(att, vid, snap_id);

    if (rc == Request::SUCCESS)
    {
        success_response(att.resp_id, att);
    }
    else
    {
        failure_response(rc, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineSnapshotDelete::request_execute(
        RequestAttributes& att,
        int vid,
        int snap_id)
{
    int rc;

    // -------------------------------------------------------------------------
    // Authorize the operation
    // -------------------------------------------------------------------------
    auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, 0);

    if (auth != SUCCESS)
    {
        return auth;
    }

    rc = dm->snapshot_delete(vid, snap_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        return ACTION;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineSnapshotDelete::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int vid     = paramList.getInt(1);
    int snap_id = paramList.getInt(2);

    auto rc = request_execute(att, vid, snap_id);

    if (rc == Request::SUCCESS)
    {
        success_response(att.resp_id, att);
    }
    else
    {
        failure_response(rc, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineAttachNic::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    VirtualMachineTemplate  tmpl;

    int     rc;

    int     id       = xmlrpc_c::value_int(paramList.getInt(1));
    string  str_tmpl = xmlrpc_c::value_string(paramList.getString(2));

    // -------------------------------------------------------------------------
    // Check if the VM is a Virtual Router
    // -------------------------------------------------------------------------
    if (auto vm = get_vm(id, att))
    {
        if (vm->is_vrouter())
        {
            att.resp_msg = "Action is not supported for virtual router VMs";
            failure_response(Request::ACTION, att);

            return;
        }
    }
    else
    {
        return;
    }

    // -------------------------------------------------------------------------
    // Parse NIC template
    // -------------------------------------------------------------------------
    rc = tmpl.parse_str_or_xml(str_tmpl, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(INTERNAL, att);
        return;
    }

    // -------------------------------------------------------------------------
    // Perform the attach
    // -------------------------------------------------------------------------
    ErrorCode ec = request_execute(id, tmpl, att);

    if ( ec == SUCCESS )
    {
        success_response(id, att);
    }
    else
    {
        failure_response(ec, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineAttachNic::request_execute(int id,
                                                            VirtualMachineTemplate& tmpl, RequestAttributes& att)
{
    Nebula& nd = Nebula::instance();

    HostPool * hpool    = nd.get_hpool();

    VirtualMachinePool* vmpool = nd.get_vmpool();

    PoolObjectAuth vm_perms;

    int hid = -1;

    // -------------------------------------------------------------------------
    // Authorize the operation, restricted attributes & check quotas
    // -------------------------------------------------------------------------
    VectorAttribute * pci = tmpl.get("PCI");

    if (auto vm = vmpool->get_ro(id))
    {
        vm->get_permissions(vm_perms);

        if (vm->hasHistory())
        {
            hid = vm->get_hid();
        }

        if (pci != nullptr && vm->check_pci_attributes(pci,  att.resp_msg) != 0)
        {
            return ACTION;
        }
    }
    else
    {
        att.resp_id  = id;
        att.resp_obj = PoolObjectSQL::VM;
        return NO_EXISTS;
    }

    AuthRequest ar(att.uid, att.group_ids);

    ar.add_auth(att.auth_op, vm_perms);

    VirtualMachine::set_auth_request(att.uid, ar, &tmpl, true);

    if (UserPool::authorize(ar) == -1)
    {
        att.resp_msg = ar.message;
        return AUTHORIZATION;
    }

    RequestAttributes att_quota(vm_perms.uid, vm_perms.gid, att);

    if (!att.is_admin())
    {
        string aname;

        if (tmpl.check_restricted(aname))
        {
            att.resp_msg = "NIC includes a restricted attribute " + aname;
            return AUTHORIZATION;
        }
    }

    if (quota_authorization(&tmpl, Quotas::NETWORK, att_quota,
                            att.resp_msg) == false)
    {
        return AUTHORIZATION;
    }

    // -------------------------------------------------------------------------
    // PCI test and set
    // -------------------------------------------------------------------------
    HostShareCapacity sr;

    if ( pci != nullptr && hid != -1 )
    {
        if ( pci->vector_value("TYPE") != "NIC" )
        {
            att.resp_msg = "PCI device is not of type NIC";

            quota_rollback(&tmpl, Quotas::NETWORK, att_quota);
            return ACTION;
        }

        sr.vmid = id;
        sr.pci.push_back(pci);

        auto host = hpool->get(hid);

        if ( host == nullptr )
        {
            att.resp_id  = id;
            att.resp_obj = PoolObjectSQL::HOST;

            quota_rollback(&tmpl, Quotas::NETWORK, att_quota);
            return NO_EXISTS;
        }

        if (!host->add_pci(sr))
        {
            att.resp_msg = "Cannot assign PCI device in host. Check address "
                           "and free devices";

            quota_rollback(&tmpl, Quotas::NETWORK, att_quota);
            return ACTION;
        }

        hpool->update(host.get());
    }

    // -------------------------------------------------------------------------
    // Perform the attach
    // -------------------------------------------------------------------------
    int rc = dm->attach_nic(id, &tmpl, att, att.resp_msg);

    if ( rc != 0 )
    {
        quota_rollback(&tmpl, Quotas::NETWORK, att_quota);

        if ( pci != nullptr && hid != -1 )
        {
            if (auto host = hpool->get(hid))
            {
                host->del_pci(sr);
                hpool->update(host.get());
            }
        }

        return ACTION;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDetachNic::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    int id     = xmlrpc_c::value_int(paramList.getInt(1));
    int nic_id = xmlrpc_c::value_int(paramList.getInt(2));

    // -------------------------------------------------------------------------
    // Check if the VM is a Virtual Router
    // -------------------------------------------------------------------------

    if (auto vm = get_vm(id, att))
    {
        if (vm->is_vrouter())
        {
            att.resp_msg = "Action is not supported for virtual router VMs";
            failure_response(Request::ACTION, att);
            return;
        }
    }
    else
    {
        return;
    }

    // -------------------------------------------------------------------------
    // Perform the detach
    // -------------------------------------------------------------------------
    ErrorCode ec = request_execute(id, nic_id, att);

    if ( ec == SUCCESS )
    {
        success_response(id, att);
    }
    else
    {
        failure_response(ec, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineDetachNic::request_execute(int id, int nic_id,
                                                            RequestAttributes& att)
{
    Nebula&             nd      = Nebula::instance();
    VirtualMachinePool* vmpool  = nd.get_vmpool();

    PoolObjectAuth      vm_perms;

    // -------------------------------------------------------------------------
    // Authorize the operation
    // -------------------------------------------------------------------------
    if (auto vm = vmpool->get_ro(id))
    {
        vm->get_permissions(vm_perms);
    }
    else
    {
        return NO_EXISTS;
    }

    AuthRequest ar(att.uid, att.group_ids);

    ar.add_auth(att.auth_op, vm_perms);

    if (UserPool::authorize(ar) == -1)
    {
        att.resp_msg = ar.message;
        return AUTHORIZATION;
    }

    // -------------------------------------------------------------------------
    // Perform the detach
    // -------------------------------------------------------------------------
    if ( dm->detach_nic(id, nic_id, att, att.resp_msg) != 0 )
    {
        return ACTION;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineUpdateNic::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    VirtualMachineTemplate tmpl;
    PoolObjectAuth vm_perms;

    int     rc;

    int     id       = paramList.getInt(1);
    int     nic_id   = paramList.getInt(2);
    string  str_tmpl = paramList.getString(3);

    int append = 0;

    if ( paramList.size() > 4 )
    {
        append = xmlrpc_c::value_int(paramList.getInt(4));
    }

    // -------------------------------------------------------------------------
    // Check if the VM is a Virtual Router
    // -------------------------------------------------------------------------
    if (auto vm = get_vm(id, att))
    {
        if (vm->is_vrouter())
        {
            att.resp_msg = "Action is not supported for virtual router VMs";
            failure_response(Request::ACTION, att);

            return;
        }

        vm->get_permissions(vm_perms);

        auto nic = vm->get_nic(nic_id);

        if (!nic)
        {
            att.resp_msg = "VM " + to_string(id) + ": NIC " + to_string(nic_id) +
                           " does not exists";
            failure_response(ACTION, att);

            return;
        }

        if (nic->is_alias())
        {
            att.resp_msg = "Action not supported for NIC_ALIAS";
            failure_response(ACTION, att);

            return;
        }
    }
    else
    {
        return;
    }

    // -------------------------------------------------------------------------
    // Parse NIC template
    // -------------------------------------------------------------------------
    rc = tmpl.parse_str_or_xml(str_tmpl, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(INTERNAL, att);
        return;
    }

    // -------------------------------------------------------------------------
    // Authorize the operation, restricted attributes & check quotas
    // -------------------------------------------------------------------------
    AuthRequest ar(att.uid, att.group_ids);

    ar.add_auth(att.auth_op, vm_perms);

    VirtualMachine::set_auth_request(att.uid, ar, &tmpl, true);

    if (UserPool::authorize(ar) == -1)
    {
        att.resp_msg = ar.message;
        failure_response(AUTHORIZATION, att);
        return;
    }

    if (!att.is_admin())
    {
        string aname;

        if (tmpl.check_restricted(aname))
        {
            att.resp_msg = "NIC includes a restricted attribute " + aname;
            failure_response(AUTHORIZATION, att);
            return;
        }
    }

    // -------------------------------------------------------------------------
    // Perform the update
    // -------------------------------------------------------------------------
    rc = dm->update_nic(id, nic_id, &tmpl, append, att, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(ACTION, att);
        return;
    }

    success_response(id, att);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineRecover::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    int id = xmlrpc_c::value_int(paramList.getInt(1));
    int op = xmlrpc_c::value_int(paramList.getInt(2));

    int    rc;
    string error;

    VMActions::Action action;

    switch (op)
    {
        case 0: //recover-failure
        case 1: //recover-success
            action = VMActions::RECOVER_ACTION;
            break;

        case 2: //retry
            action = VMActions::RETRY_ACTION;
            break;

        case 3: //delete
        case 5: //delete-db
            action = VMActions::DELETE_ACTION;
            break;

        case 4: //delete-recreate set same as delete in OpenNebulaTemplate
            action = VMActions::DELETE_RECREATE_ACTION;
            break;

        default:
            att.resp_msg = "Wrong recovery operation code";
            failure_response(ACTION, att);
            return;
    }

    att.set_auth_op(action);

    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    auto vm = pool->get<VirtualMachine>(id);

    if (vm == nullptr)
    {
        att.resp_id = id;
        failure_response(NO_EXISTS, att);
        return;
    }

    switch (op)
    {
        case 0: //recover-failure
            rc = dm->recover(std::move(vm), false, att, error);
            break;

        case 1: //recover-success
            rc = dm->recover(std::move(vm), true, att, error);
            break;

        case 2: //retry
            rc = dm->retry(std::move(vm), att, error);
            break;

        case 3: //delete
            rc = dm->delete_vm(std::move(vm), att, error);
            break;

        case 4: //delete-recreate
            rc = dm->delete_recreate(std::move(vm), att, error);
            break;
        case 5:
            rc = dm->delete_vm_db(std::move(vm), att, error);
            break;
    }

    if ( rc == 0 )
    {
        success_response(id, att);
    }
    else
    {
        att.resp_msg = error;
        failure_response(ACTION, att);
    }

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachinePoolCalculateShowback::request_execute(
        xmlrpc_c::paramList const& paramList,
        RequestAttributes& att)
{
    int start_month = xmlrpc_c::value_int(paramList.getInt(1));
    int start_year  = xmlrpc_c::value_int(paramList.getInt(2));
    int end_month   = xmlrpc_c::value_int(paramList.getInt(3));
    int end_year    = xmlrpc_c::value_int(paramList.getInt(4));

    int           rc;

    if ( att.gid != 0 )
    {
        att.resp_msg = "Action reserved for group 0 only";
        failure_response(AUTHORIZATION, att);
        return;
    }

    rc = (static_cast<VirtualMachinePool *>(pool))->calculate_showback(
                 start_month, start_year, end_month, end_year, att.resp_msg);

    if (rc != 0)
    {
        failure_response(AUTHORIZATION, att);
        return;
    }

    success_response(string(""), att);

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineDiskSnapshotCreate::request_execute(
        RequestAttributes& att,
        int vid,
        int disk_id,
        const std::string& name)
{
    PoolObjectAuth   vm_perms;

    VirtualMachineDisk * disk;

    Template ds_deltas;
    Template vm_deltas;

    int    rc;
    int    snap_id;

    // ------------------------------------------------------------------------
    // Check request consistency (VM & disk exists, no volatile)
    // ------------------------------------------------------------------------
    bool img_ds_quota, vm_ds_quota;
    bool is_volatile;
    int img_id = -1;

    if (auto vm = pool->get_ro<VirtualMachine>(vid))
    {
        auto vm_bck = vm->backups();

        if ( vm_bck.configured() && vm_bck.mode() == Backups::INCREMENT )
        {
            att.resp_msg = "Action \"disk-snapshot-create\" is not compatible with "
                           "incremental backups";

            return ACTION;
        }

        disk = vm->get_disk(disk_id);

        if (disk == nullptr)
        {
            att.resp_msg = "VM disk does not exist";

            return ACTION;
        }

        /* ---------------------------------------------------------------------- */
        /*  Get disk information and quota usage deltas                           */
        /* ---------------------------------------------------------------------- */
        long long ssize;
        disk->vector_value("SIZE", ssize);

        // Snapshot accounts as another disk of same size
        disk->resize_quotas(ssize, ds_deltas, vm_deltas, img_ds_quota, vm_ds_quota);
        if (vm->hasHistory() && !vm_deltas.empty())
        {
            vm_deltas.add("CLUSTER_ID", vm->get_cid());
        }

        is_volatile = disk->is_volatile();

        disk->vector_value("IMAGE_ID", img_id);

        vm->get_permissions(vm_perms);
    }
    else
    {
        att.resp_id = vid;
        return NO_EXISTS;
    }

    if (is_volatile)
    {
        att.resp_msg = "Cannot make snapshots on volatile disks";
        return ACTION;
    }

    /* ---------- Attributes for quota update requests ---------------------- */
    PoolObjectAuth img_perms;

    if (img_ds_quota)
    {
        if ( auto img = ipool->get_ro(img_id) )
        {
            img->get_permissions(img_perms);
        }
        else
        {
            att.resp_obj = PoolObjectSQL::IMAGE;
            att.resp_id  = img_id;

            return NO_EXISTS;
        }

        auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, &img_perms);

        if (auth != SUCCESS)
        {
            return auth;
        }
    }
    else
    {
        auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, 0);

        if (auth != SUCCESS)
        {
            return auth;
        }
    }

    RequestAttributes vm_att_quota  = RequestAttributes(vm_perms.uid,
                                                        vm_perms.gid, att);
    RequestAttributes img_att_quota = RequestAttributes(img_perms.uid,
                                                        img_perms.gid, att);

    /* ---------------------------------------------------------------------- */
    /*  Check quotas for the new size in image/system datastoress             */
    /* ---------------------------------------------------------------------- */
    if ( img_ds_quota &&
         !quota_authorization(&ds_deltas, Quotas::DATASTORE, img_att_quota, att.resp_msg) )
    {
        return AUTHORIZATION;
    }

    if ( vm_ds_quota &&
         !quota_authorization(&ds_deltas, Quotas::DATASTORE, vm_att_quota, att.resp_msg) )
    {
        if ( img_ds_quota )
        {
            quota_rollback(&ds_deltas, Quotas::DATASTORE, img_att_quota);
        }

        return AUTHORIZATION;
    }

    if ( !vm_deltas.empty() )
    {
        if (!quota_resize_authorization(&vm_deltas, vm_att_quota, vm_perms))
        {
            if ( img_ds_quota )
            {
                quota_rollback(&ds_deltas, Quotas::DATASTORE, img_att_quota);
            }

            if ( vm_ds_quota )
            {
                quota_rollback(&ds_deltas, Quotas::DATASTORE, vm_att_quota);
            }

            att.resp_msg = vm_att_quota.resp_msg;
            return AUTHORIZATION;
        }
    }

    // ------------------------------------------------------------------------
    // Do the snapshot
    // ------------------------------------------------------------------------
    rc = dm->disk_snapshot_create(vid, disk_id, name, snap_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        if ( img_ds_quota )
        {
            quota_rollback(&ds_deltas, Quotas::DATASTORE, img_att_quota);
        }

        if ( vm_ds_quota )
        {
            quota_rollback(&ds_deltas, Quotas::DATASTORE, vm_att_quota);
        }

        if ( !vm_deltas.empty() )
        {
            quota_rollback(&vm_deltas, Quotas::VM, vm_att_quota);
        }

        return ACTION;
    }

    att.resp_id = snap_id;
    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDiskSnapshotCreate::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int    vid  = paramList.getInt(1);
    int    did  = paramList.getInt(2);
    string name = paramList.getString(3);

    auto rc = request_execute(att, vid, did, name);

    if (rc == Request::SUCCESS)
    {
        success_response(att.resp_id, att);
    }
    else
    {
        failure_response(rc, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineDiskSnapshotRevert::request_execute(
        RequestAttributes& att,
        int vid,
        int disk_id,
        int snap_id)
{
    int    rc;

    auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, 0);

    if (auth != SUCCESS)
    {
        return auth;
    }

    rc = dm->disk_snapshot_revert(vid, disk_id, snap_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        return ACTION;
    }

    att.resp_id = vid;
    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDiskSnapshotRevert::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int vid     = paramList.getInt(1);
    int disk_id = paramList.getInt(2);
    int snap_id = paramList.getInt(3);

    auto rc = request_execute(att, vid, disk_id, snap_id);

    if (rc == Request::SUCCESS)
    {
        success_response(att.resp_id, att);
    }
    else
    {
        failure_response(rc, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineDiskSnapshotDelete::request_execute(
        RequestAttributes& att,
        int vid,
        int disk_id,
        int snap_id)
{
    const VirtualMachineDisk * disk;

    int rc;

    bool persistent;
    int img_id = -1;

    if (auto vm = pool->get_ro<VirtualMachine>(vid))
    {
        disk = vm->get_disk(disk_id);

        if (disk == nullptr)
        {
            att.resp_msg = "VM disk does not exist";

            return ACTION;
        }

        persistent = disk->is_persistent();

        disk->vector_value("IMAGE_ID", img_id);
    }
    else
    {
        att.resp_id = vid;
        return NO_EXISTS;
    }

    if (persistent)
    {
        PoolObjectAuth img_perms;

        auto img = ipool->get_ro(img_id);

        if (img == nullptr)
        {
            att.resp_obj = PoolObjectSQL::IMAGE;
            att.resp_id  = img_id;

            return NO_EXISTS;
        }

        img->get_permissions(img_perms);

        auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, &img_perms);

        if (auth != SUCCESS)
        {
            return auth;
        }
    }
    else
    {
        auto auth = vm_authorization_no_response(vid, 0, 0, att, 0, 0, 0);

        if (auth != SUCCESS)
        {
            return auth;
        }
    }

    rc = dm->disk_snapshot_delete(vid, disk_id, snap_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        return ACTION;
    }

    att.resp_id = vid;
    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDiskSnapshotDelete::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int vid     = paramList.getInt(1);
    int disk_id = paramList.getInt(2);
    int snap_id = paramList.getInt(3);

    auto rc = request_execute(att, vid, disk_id, snap_id);

    if (rc == Request::SUCCESS)
    {
        success_response(att.resp_id, att);
    }
    else
    {
        failure_response(rc, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDiskSnapshotRename::request_execute(xmlrpc_c::paramList const& paramList,
                                                       RequestAttributes& att)
{
    ostringstream oss;

    const VirtualMachineDisk * disk;

    int rc;

    int id               = xmlrpc_c::value_int(paramList.getInt(1));
    int did              = xmlrpc_c::value_int(paramList.getInt(2));
    int snap_id          = xmlrpc_c::value_int(paramList.getInt(3));
    string new_snap_name = xmlrpc_c::value_string(paramList.getString(4));

    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    auto vm = get_vm(id, att);
    if (vm == nullptr)
    {
        oss << "Could not rename the snapshot for VM " << id
            << ", VM does not exist";

        att.resp_msg = oss.str();

        return;
    }

    disk = vm->get_disk(did);

    if (disk == nullptr)
    {
        att.resp_msg = "VM disk does not exist";
        failure_response(ACTION, att);

        return;
    }

    rc = vm->rename_disk_snapshot(did, snap_id, new_snap_name, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(ACTION, att);
        return;
    }

    success_response(id, att);

    pool->update(vm.get());

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineUpdateConf::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    int     id       = xmlrpc_c::value_int(paramList.getInt(1));
    string  str_tmpl = xmlrpc_c::value_string(paramList.getString(2));
    int     update_type = 0;

    if ( paramList.size() > 3 )
    {
        update_type = xmlrpc_c::value_int(paramList.getInt(3));
    }

    if ( update_type < 0 || update_type > 1 )
    {
        att.resp_msg = "Wrong update type";
        failure_response(XML_RPC_API, att);
        return;
    }

    VirtualMachineTemplate tmpl;
    VirtualMachinePool * vmpool = static_cast<VirtualMachinePool *>(pool);

    // -------------------------------------------------------------------------
    // Parse template
    // -------------------------------------------------------------------------
    int rc = tmpl.parse_str_or_xml(str_tmpl, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(INTERNAL, att);
        return;
    }

    auto uc_tmpl = tmpl.get_updateconf_template();

    /* ---------------------------------------------------------------------- */
    /*  Authorize the operation & restricted attributes                       */
    /* ---------------------------------------------------------------------- */
    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    /* ---------------------------------------------------------------------- */
    /* Update VirtualMachine Configuration                                    */
    /* ---------------------------------------------------------------------- */
    auto vm = vmpool->get(id);

    if (vm == nullptr)
    {
        att.resp_id = id;
        failure_response(NO_EXISTS, att);
        return;
    }

    if (!att.is_admin())
    {
        string aname;

        if ( vm->check_restricted(aname, uc_tmpl.get(), true) )
        {
            att.resp_msg = "Template includes a restricted attribute " + aname;
            failure_response(AUTHORIZATION, att);

            return;
        }
    }

    rc = vm->updateconf(uc_tmpl.get(), att.resp_msg, update_type == 1);

    // rc = -1 (error), 0 (context changed), 1 (no context change)
    if ( rc == -1 )
    {
        failure_response(INTERNAL, att);

        return;
    }

    /* ---------------------------------------------------------------------- */
    /* Update PORT if a new GRAPHICS section has been added & VM is deployed. */
    /* ---------------------------------------------------------------------- */
    VectorAttribute * graphics = vm->get_template_attribute("GRAPHICS");

    unsigned int port;

    VirtualMachine::VmState  state     = vm->get_state();
    VirtualMachine::LcmState lcm_state = vm->get_lcm_state();

    if (graphics != nullptr && graphics->vector_value("PORT", port) == -1 &&
        (state == VirtualMachine::ACTIVE || state == VirtualMachine::POWEROFF ||
         state == VirtualMachine::SUSPENDED))
    {
        ClusterPool * cpool = Nebula::instance().get_clpool();

        rc = cpool->get_vnc_port(vm->get_cid(), vm->get_oid(), port);

        if ( rc != 0 )
        {
            att.resp_msg = "No free VNC ports available in the cluster";
            failure_response(INTERNAL, att);

            return;
        }

        graphics->replace("PORT", port);
    }

    vmpool->update(vm.get());

    // Apply the change for running VM
    if (state == VirtualMachine::VmState::ACTIVE &&
        lcm_state == VirtualMachine::LcmState::RUNNING && rc == 0)
    {
        auto dm = Nebula::instance().get_dm();

        if (dm->live_updateconf(std::move(vm), att, att.resp_msg) != 0)
        {
            failure_response(INTERNAL, att);

            return;
        }
    }

    success_response(id, att);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDiskResize::request_execute(
        xmlrpc_c::paramList const&  paramList, RequestAttributes& att)
{
    Template ds_deltas;
    Template vm_deltas;

    PoolObjectAuth   vm_perms;

    int    id     = xmlrpc_c::value_int(paramList.getInt(1));
    int    did    = xmlrpc_c::value_int(paramList.getInt(2));
    string size_s = xmlrpc_c::value_string(paramList.getString(3));

    long long size, current_size;

    // ------------------------------------------------------------------------
    // Check request consistency (VM & disk exists, size, and no snapshots)
    // ------------------------------------------------------------------------
    istringstream iss(size_s);

    iss >> size;

    if (iss.fail() || !iss.eof())
    {
        att.resp_msg = "Disk SIZE is not a valid integer";
        failure_response(ACTION, att);

        return;
    }

    // -------------------------------------------------------------------------
    // Authorize the operation
    // -------------------------------------------------------------------------
    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    bool img_ds_quota, vm_ds_quota;
    int img_id = -1;

    if ( auto vm = get_vm(id, att) )
    {
        VirtualMachineDisk * disk = vm->get_disk(did);

        if (disk == nullptr)
        {
            att.resp_msg = "VM disk does not exist";
            failure_response(ACTION, att);

            return;
        }

        disk->vector_value("SIZE", current_size);

        if ( size <= current_size )
        {
            att.resp_msg = "New disk size has to be greater than current one";
            failure_response(ACTION, att);

            return;
        }

        /* ------------- Get information about the disk and image --------------- */
        disk->resize_quotas(size - current_size, ds_deltas, vm_deltas, img_ds_quota, vm_ds_quota);
        if (vm->hasHistory() && !vm_deltas.empty())
        {
            vm_deltas.add("CLUSTER_ID", vm->get_cid());
        }

        disk->vector_value("IMAGE_ID", img_id);

        vm->get_permissions(vm_perms);
    }
    else
    {
        return;
    }

    /* ---------------------------------------------------------------------- */
    /*  Authorize the request for VM and IMAGE for persistent disks           */
    /* ---------------------------------------------------------------------- */
    PoolObjectAuth img_perms;

    if ( img_ds_quota )
    {
        if ( img_id != -1 )
        {
            auto img = ipool->get_ro(img_id);

            if (img == nullptr)
            {
                att.resp_obj = PoolObjectSQL::IMAGE;
                att.resp_id  = img_id;
                failure_response(NO_EXISTS, att);

                return;
            }

            img->get_permissions(img_perms);
        }

        if (vm_authorization(id, 0, 0, att, 0, 0, &img_perms) == false)
        {
            return;
        }
    }

    if ( vm_ds_quota )
    {
        if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
        {
            return;
        }
    }

    RequestAttributes img_att_quota = RequestAttributes(img_perms.uid,
                                                        img_perms.gid, att);
    RequestAttributes vm_att_quota  = RequestAttributes(vm_perms.uid,
                                                        vm_perms.gid, att);

    /* ---------------------------------------------------------------------- */
    /*  Check quotas for the new size in image/system datastoress             */
    /* ---------------------------------------------------------------------- */

    if ( img_ds_quota && !quota_authorization(&ds_deltas, Quotas::DATASTORE,
                                              img_att_quota))
    {
        return;
    }

    if ( vm_ds_quota && !quota_authorization(&ds_deltas, Quotas::DATASTORE,
                                             vm_att_quota))
    {
        if ( img_ds_quota )
        {
            quota_rollback(&ds_deltas, Quotas::DATASTORE, img_att_quota);
        }

        return;
    }

    if ( !vm_deltas.empty() )
    {
        if (!quota_resize_authorization(&vm_deltas, vm_att_quota, vm_perms))
        {
            if ( img_ds_quota )
            {
                quota_rollback(&ds_deltas, Quotas::DATASTORE, img_att_quota);
            }

            if ( vm_ds_quota )
            {
                quota_rollback(&ds_deltas, Quotas::DATASTORE, vm_att_quota);
            }

            failure_response(AUTHORIZATION, vm_att_quota);
            return;
        }
    }

    // ------------------------------------------------------------------------
    // Resize the disk
    // ------------------------------------------------------------------------
    int rc = dm->disk_resize(id, did, size, att, att.resp_msg);

    if ( rc != 0 )
    {
        if ( img_ds_quota )
        {
            quota_rollback(&ds_deltas, Quotas::DATASTORE, img_att_quota);
        }

        if ( vm_ds_quota )
        {
            quota_rollback(&ds_deltas, Quotas::DATASTORE, vm_att_quota);
        }

        if ( !vm_deltas.empty() )
        {
            quota_rollback(&vm_deltas, Quotas::VM, vm_att_quota);
        }

        failure_response(ACTION, att);
    }
    else
    {
        success_response(did, att);
    }

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineAttachSG::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int vm_id  = xmlrpc_c::value_int(paramList.getInt(1));
    int nic_id = xmlrpc_c::value_int(paramList.getInt(2));
    int sg_id  = xmlrpc_c::value_int(paramList.getInt(3));

    unique_ptr<VirtualMachineNic> nic_tmpl;

    PoolObjectAuth vm_perms;

    // Get VM attributes to authorize operation
    if (auto vm = get_vm_ro(vm_id, att))
    {
        // Check if we can add the SG
        auto nic = vm->get_nic(nic_id);

        if (!nic)
        {
            ostringstream oss;
            oss << "VM " << vm_id << " doesn't have NIC id " << nic_id;
            att.resp_msg = oss.str();

            failure_response(Request::INTERNAL, att);
            return;
        }

        // Copy VM attributes
        nic_tmpl.reset(new VirtualMachineNic(nic->vector_attribute()->clone(), nic_id));
        nic_tmpl->add_security_group(sg_id);

        vm->get_permissions(vm_perms);
    }
    else
    {
        return;
    }

    // Authorize the operation
    AuthRequest ar(att.uid, att.group_ids);

    ar.add_auth(att.auth_op, vm_perms);

    nic_tmpl->authorize(att.uid, &ar, true);

    if (UserPool::authorize(ar) == -1)
    {
        att.resp_msg = ar.message;
        failure_response(Request::AUTHORIZATION, att);
        return;
    }

    int rc = dm->attach_sg(vm_id, nic_id, sg_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(ACTION, att);
    }
    else
    {
        success_response(vm_id, att);
    }

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineDetachSG::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    int vm_id  = xmlrpc_c::value_int(paramList.getInt(1));
    int nic_id = xmlrpc_c::value_int(paramList.getInt(2));
    int sg_id  = xmlrpc_c::value_int(paramList.getInt(3));

    if (!vm_authorization(vm_id, 0, 0, att, 0, 0, 0))
    {
        return;
    }

    auto rc = dm->detach_sg(vm_id, nic_id, sg_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(ACTION, att);
    }
    else
    {
        success_response(vm_id, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineBackup::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    // ------------------------------------------------------------------------
    // Get request parameters
    // ------------------------------------------------------------------------
    int  vm_id        = xmlrpc_c::value_int(paramList.getInt(1));
    int  backup_ds_id = -1;
    bool reset        = false;

    if ( paramList.size() > 2 )
    {
        backup_ds_id = xmlrpc_c::value_int(paramList.getInt(2));
    }

    if ( paramList.size() > 3 )
    {
        reset = xmlrpc_c::value_boolean(paramList.getBoolean(3));
    }

    if ( auto vm = pool->get_ro<VirtualMachine>(vm_id) )
    {
        int bj_id = vm->backups().backup_job_id();

        if ( bj_id != -1)
        {
            att.resp_msg = "Unable to start an individual backup for the Virtual Machine"
                           ", it is part of Backup Job ID " + to_string(bj_id);

            failure_response(INTERNAL, att);
            return;
        }
    }
    else
    {
        att.resp_id  = vm_id;

        failure_response(NO_EXISTS, att);
        return;
    }

    auto ec = request_execute(att, vm_id, backup_ds_id, reset);

    if ( ec == Request::SUCCESS)
    {
        success_response(vm_id, att);
    }
    else
    {
        failure_response(ec, att);
    }
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

Request::ErrorCode VirtualMachineBackup::request_execute(RequestAttributes& att,
                                                         int vm_id,
                                                         int backup_ds_id,
                                                         bool reset)
{
    Nebula&            nd = Nebula::instance();
    DatastorePool *    dspool = nd.get_dspool();

    PoolObjectAuth vm_perms;
    PoolObjectAuth ds_perms;
    Template       quota_tmpl;

    Backups::Mode mode;
    int li_id;
    int bk_id = -1;
    long long backup_size = 0;

    // ------------------------------------------------------------------------
    // Get VM & Backup Information
    // ------------------------------------------------------------------------
    if ( auto vm = pool->get<VirtualMachine>(vm_id) )
    {
        vm->get_permissions(vm_perms);

        backup_size = vm->backup_size(quota_tmpl);

        mode  = vm->backups().mode();
        li_id = vm->backups().last_increment_id();

        bk_id = vm->backups().incremental_backup_id();
    }
    else
    {
        att.resp_id  = vm_id;

        return NO_EXISTS;
    }

    // Incremental backups use the current datastore if not resetting the chain
    if ( mode == Backups::INCREMENT && !reset && li_id != -1 )
    {
        ImagePool* ipool = nd.get_ipool();

        if (auto img = ipool->get_ro(bk_id))
        {
            backup_ds_id = img->get_ds_id();
        }
        else
        {
            att.resp_obj = PoolObjectSQL::IMAGE;
            att.resp_id  = bk_id;

            return NO_EXISTS;
        }
    }

    if ( auto ds = dspool->get_ro(backup_ds_id) )
    {
        if (ds->get_type() != Datastore::BACKUP_DS)
        {
            att.resp_msg = "Datastore needs to be of type BACKUP";

            return ACTION;
        }

        long long free_mb;
        bool check_capacity = ds->get_avail_mb(free_mb);

        if (check_capacity && free_mb < backup_size)
        {
            att.resp_msg = "Not enough free space on target datastore";

            return ACTION;
        }

        ds->get_permissions(ds_perms);
    }
    else
    {
        att.resp_obj = PoolObjectSQL::DATASTORE;
        att.resp_id  = backup_ds_id;

        return NO_EXISTS;
    }

    // ------------------------------------------------------------------------
    // Authorize request (VM and Datastore access)
    // ------------------------------------------------------------------------
    auto auth = vm_authorization_no_response(vm_id, 0, 0, att, 0, &ds_perms, 0);

    if (auth != SUCCESS)
    {
        return auth;
    }

    // -------------------------------------------------------------------------
    // Check backup datastore quotas (size or number of backups)
    //
    // Reserves maximal possible quota size for the backup, the value is updated
    // after backup success notification from driver
    // -------------------------------------------------------------------------
    quota_tmpl.add("DATASTORE", backup_ds_id);

    if ( mode == Backups::FULL || li_id == -1 || reset )
    {
        quota_tmpl.add("IMAGES", 1);
    }
    else
    {
        quota_tmpl.add("IMAGES", 0);
    }


    RequestAttributes att_quota(vm_perms.uid, vm_perms.gid, att);

    if ( !quota_authorization(&quota_tmpl, Quotas::DATASTORE, att_quota, att_quota.resp_msg) )
    {
        att.resp_msg = att_quota.resp_msg;

        return AUTHORIZATION;
    }

    // ------------------------------------------------------------------------
    // Create backup
    // ------------------------------------------------------------------------
    if (dm->backup(vm_id, backup_ds_id, reset, att, att.resp_msg) < 0)
    {
        quota_rollback(&quota_tmpl, Quotas::DATASTORE, att_quota);

        return INTERNAL;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineBackupCancel::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    // Get request parameters
    int vm_id = xmlrpc_c::value_int(paramList.getInt(1));

    // Authorize request (VM access)
    if (!vm_authorization(vm_id, 0, 0, att, 0, 0, 0))
    {
        return;
    }

    // Cancel the backup, VM state is checked in DM
    if (dm->backup_cancel(vm_id, att, att.resp_msg) != 0)
    {
        failure_response(INTERNAL, att);
        return;
    }

    success_response(vm_id, att);

    return;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void VirtualMachineAttachPCI::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    int    id    = xmlrpc_c::value_int(paramList.getInt(1));
    string stmpl = xmlrpc_c::value_string(paramList.getString(2));

    // -------------------------------------------------------------------------
    // Parse PCI template and check PCI attributes
    // -------------------------------------------------------------------------
    VirtualMachineTemplate  tmpl;

    int rc = tmpl.parse_str_or_xml(stmpl, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(INTERNAL, att);
        return;
    }

    VectorAttribute * pci = tmpl.get("PCI");

    if ( pci == nullptr )
    {
        att.resp_msg = "PCI attribute not found";

        failure_response(ACTION, att);
        return;
    }
    else if ( pci->vector_value("TYPE") == "NIC" )
    {
        att.resp_msg = "Use one.vm.attachnic to attach this PCI device";

        failure_response(ACTION, att);
        return;
    }
    else if ( VirtualMachine::check_pci_attributes(pci, att.resp_msg) != 0)
    {
        failure_response(ACTION, att);
        return;
    }

    // -------------------------------------------------------------------------
    // Authorize the operation, restricted attributes
    // -------------------------------------------------------------------------
    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    if (!att.is_admin())
    {
        string aname;

        if (tmpl.check_restricted(aname))
        {
            att.resp_msg = "PCI includes a restricted attribute " + aname;
            failure_response(AUTHORIZATION, att);
            return;
        }
    }

    // -------------------------------------------------------------------------
    // Perform the attach
    // -------------------------------------------------------------------------
    rc = dm->attach_pci(id, pci, att, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(ACTION, att);
    }
    else
    {
        success_response(id, att);
    }

    return;
}

// -----------------------------------------------------------------------------

void VirtualMachineDetachPCI::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    int id     = xmlrpc_c::value_int(paramList.getInt(1));
    int pci_id = xmlrpc_c::value_int(paramList.getInt(2));

    // -------------------------------------------------------------------------
    // Authorize the operation, restricted attributes
    // -------------------------------------------------------------------------
    if (vm_authorization(id, 0, 0, att, 0, 0, 0) == false)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // Perform the detach
    // -------------------------------------------------------------------------
    int rc = dm->detach_pci(id, pci_id, att, att.resp_msg);

    if ( rc != 0 )
    {
        failure_response(ACTION, att);
    }
    else
    {
        success_response(id, att);
    }
}

// -----------------------------------------------------------------------------

void VirtualMachineRestore::request_execute(
        xmlrpc_c::paramList const& paramList, RequestAttributes& att)
{
    // Get request parameters
    int vm_id   = paramList.getInt(1);
    int img_id  = paramList.getInt(2);
    int inc_id  = paramList.getInt(3);
    int disk_id = paramList.getInt(4);

    Nebula& nd = Nebula::instance();
    ImagePool *ipool  = nd.get_ipool();

    // Authorize request
    PoolObjectAuth vm_perms, img_perms;

    if (auto vm = get_vm_ro(vm_id, att))
    {
        vm->get_permissions(vm_perms);

        if (disk_id != -1 && !vm->get_disk(disk_id))
        {
            att.resp_msg = "VM disk does not exist";

            failure_response(ACTION, att);
            return;
        }
    }
    else
    {
        // Failure response set in get_vm_ro method
        return;
    }

    if (auto img = ipool->get_ro(img_id))
    {
        if (img->get_type() != Image::BACKUP)
        {
            att.resp_msg = "Image has to be of type BACKUP";

            failure_response(ACTION, att);
            return;
        }

        if (inc_id > img->last_increment_id())
        {
            att.resp_msg = "Wrong increment";

            failure_response(ACTION, att);
            return;
        }

        img->get_permissions(img_perms);
    }
    else
    {
        att.resp_obj = PoolObjectSQL::IMAGE;
        att.resp_id  = img_id;

        failure_response(NO_EXISTS, att);
        return;
    }

    AuthRequest ar(att.uid, att.group_ids);

    ar.add_auth(att.auth_op, vm_perms);
    ar.add_auth(AuthRequest::USE, img_perms);

    if (UserPool::authorize(ar) == -1)
    {
        att.resp_msg = ar.message;

        failure_response(AUTHORIZATION, att);
        return;
    }

    if (dm->restore(vm_id, img_id, inc_id, disk_id, att, att.resp_msg) != 0)
    {
        failure_response(ACTION, att);
        return;
    }

    success_response(vm_id, att);

    return;
}
