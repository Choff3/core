<?php

/*
 * Copyright (C) 2014-2016 Deciso B.V.
 * Copyright (C) 2010 Ermal Luçi
 * Copyright (C) 2007-2008 Scott Ullrich <sullrich@gmail.com>
 * Copyright (C) 2005-2006 Bill Marquette <bill.marquette@gmail.com>
 * Copyright (C) 2006 Paul Taylor <paultaylor@winn-dixie.com>
 * Copyright (C) 2003-2006 Manuel Kasper <mk@neon1.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

require_once("interfaces.inc");
require_once("util.inc");

$groupindex = index_groups();
$userindex = index_users();

/**
 * check if $http_host is a local configured ip address
 */
function isAuthLocalIP($http_host)
{
    global $config;
    if (isset($config['virtualip']['vip'])) {
        foreach ($config['virtualip']['vip'] as $vip) {
            if ($vip['subnet'] == $http_host) {
                return true;
            }
        }
    }
    $address_in_list = function ($interface_list_ips, $http_host) {
        foreach ($interface_list_ips as $ilips => $ifname) {
            // remove scope from link-local IPv6 addresses
            $ilips = preg_replace('/%.*/', '', $ilips);
            if (strcasecmp($http_host, $ilips) == 0) {
                return true;
            }
        }
    };
    // try using cached addresses
    $interface_list_ips = get_cached_json_content("/tmp/isAuthLocalIP.cache.json");
    if (!empty($interface_list_ips) && $address_in_list($interface_list_ips, $http_host)) {
        return true;
    }
    // fetch addresses and store in cache
    $interface_list_ips = get_configured_ip_addresses();
    file_put_contents("/tmp/isAuthLocalIP.cache.json", json_encode($interface_list_ips));

    return $address_in_list($interface_list_ips, $http_host);
}

function index_groups()
{
    global $config, $groupindex;

    $groupindex = array();
    if (isset($config['system']['group'])) {
        $i = 0;
        foreach ($config['system']['group'] as $groupent) {
            if (isset($groupent['name'])) {
                $groupindex[$groupent['name']] = $i;
                $i++;
            }
        }
    }

    return ($groupindex);
}

function index_users()
{
    global $config;

    $userindex = array();

    if (!empty($config['system']['user']) && is_array($config['system']['user'])) {
        $i = 0;
        foreach ($config['system']['user'] as $userent) {
            $userindex[$userent['name']] = $i;
            $i++;
        }
    }

    return $userindex;
}

function getUserGroups($username)
{
    global $config;
    $member_groups = array();
    $user = getUserEntry($username);
    if ($user !== false) {
        $allowed_groups = local_user_get_groups($user);
        if (isset($config['system']['group'])) {
            foreach ($config['system']['group'] as $group) {
                if (in_array($group['name'], $allowed_groups)) {
                    $member_groups[] = $group['name'];
                }
            }
        }
    }
    return $member_groups;
}

function &getUserEntry($name)
{
    global $config, $userindex;
    $false = false;
    if (isset($userindex[$name])) {
        return $config['system']['user'][$userindex[$name]];
    } else {
        return $false;
    }
}

function &getUserEntryByUID($uid)
{
    global $config;

    if (is_array($config['system']['user'])) {
        foreach ($config['system']['user'] as & $user) {
            if ($user['uid'] == $uid) {
                return $user;
            }
        }
    }

    return false;
}

function &getGroupEntry($name)
{
    global $config, $groupindex;

    if (isset($groupindex[$name])) {
        return $config['system']['group'][$groupindex[$name]];
    }

    return array();
}

function get_user_privileges(&$user)
{
    if (!isset($user['priv']) || !is_array($user['priv'])) {
        $privs = array();
    } else {
        $privs = $user['priv'];
    }

    $names = local_user_get_groups($user);

    foreach ($names as $name) {
        $group = getGroupEntry($name);
        if (isset($group['priv']) && is_array($group['priv'])) {
            $privs = array_merge($privs, $group['priv']);
        }
    }
    return $privs;
}

function userHasPrivilege($userent, $privid = false)
{
    if (!$privid || !is_array($userent)) {
        return false;
    }

    $privs = get_user_privileges($userent);

    if (!is_array($privs)) {
        return false;
    }

    if (!in_array($privid, $privs)) {
        return false;
    }
    return true;
}

function userIsAdmin($username)
{
    $user = getUserEntry($username);

    return userHasPrivilege($user, 'page-all');
}

function auth_get_shells($uid = 0)
{
    $shells = array('' => '/sbin/nologin');

    if ($uid == 0) {
        $shells = array('' => '/usr/local/sbin/opnsense-shell');
    }

    $etc_shells = @file_get_contents('/etc/shells');
    if (!empty($etc_shells)) {
        $etc_shells = explode("\n", $etc_shells);
        foreach ($etc_shells as $shell) {
            $shell = trim($shell);
            if (
                !empty($shell) && strpos($shell, '#') !== 0 &&
                strpos($shell, '/usr/local/sbin/opnsense-') !== 0
            ) {
                $shells[$shell] = $shell;
            }
        }
    }

    return $shells;
}

function local_sync_accounts()
{
    global $config;

    /* we need to know the current state of users and groups, both configured and actual */
    $current_data = [];
    foreach (['usershow', 'groupshow'] as $command) {
        $section = $command == 'usershow' ? 'user' : 'group';
        $current_data[$section] = [];
        $data = "";
        exec("/usr/sbin/pw {$command} -a", $data, $ret);
        if (!$ret) {
            foreach ($data as $record) {
                $line = explode(':', $record);
                // filter system managed users and groups
                if (count($line) < 3 ||  !strncmp($line[0], '_', 1) || ($line[2] < 2000 && $line[0] != 'root') || $line[2] > 65000) {
                    continue;
                }
                $current_data[$section][$line[2]] = $line;
            }
        }
    }
    $config_map = [];
    foreach (['user', 'group'] as $section) {
        $config_map[$section] = [];
        if (is_array($config['system'][$section])) {
            foreach ($config['system'][$section] as $item) {
                $this_id = $section == 'user' ? $item['uid'] : $item['gid'];
                $config_map[$section][(string)$this_id] = $item['name'];
            }
        }
    }
    /* remove conflicting users and groups (uid/gid or name mismatch) */
    foreach ($current_data as $section => $data) {
        foreach ($data as $oid => $object) {
            if (empty($config_map[$section][$oid]) || $config_map[$section][$oid] !== $object[0]) {
                if ($section == 'user') {
                    /*
                     * If a crontab was created to user, pw userdel will be interactive and
                     * can cause issues. Just remove crontab before run it when necessary
                     */
                    @unlink("/var/cron/tabs/{$object[0]}");
                    mwexecf('/usr/sbin/pw userdel -n %s', $object[0]);
                } else {
                    mwexecf('/usr/sbin/pw groupdel -g %s', $object[2]);
                }
            }
        }
    }

    /* sync all local users */
    if (is_array($config['system']['user'])) {
        foreach ($config['system']['user'] as $user) {
            $userattrs = !empty($current_data['user'][$user['uid']]) ? $current_data['user'][$user['uid']] : [];
            local_user_set($user, false, $userattrs);
        }
    }

    /* sync all local groups */
    if (is_array($config['system']['group'])) {
        foreach ($config['system']['group'] as $group) {
            local_group_set($group);
        }
    }
}

function local_user_set(&$user, $force_password = false, $userattrs = null)
{
    global $config;

    if (empty($user['password'])) {
        log_msg(sprintf(
            gettext('There is something wrong in your config because user %s password is missing!'),
            $user['name']
        ), LOG_ERR);
        return;
    }

    @mkdir('/home', 0755);

    $user_pass = $user['password'];
    $user_name = $user['name'];
    $user_uid = $user['uid'];
    $comment = str_replace(array(':', '!', '@'), ' ', $user['descr']);

    $lock_account = 'lock';

    $is_expired = !empty($user['expires']) &&
        strtotime('-1 day') > strtotime(date('m/d/Y', strtotime($user['expires'])));

    $is_disabled = isset($user['disabled']);

    $is_unlocked = !$is_disabled && !$is_expired;

    if ($is_unlocked) {
        $lock_account = 'unlock';
    } elseif ($user_uid == 0) {
        /*
         * The root account should not be locked as this will have
         * side-effects such as cron not working correctly.  Set
         * password to unreachable "*" instead.  Our auth framework
         * already checks for disabled elsewhere so we only need to
         * prevent root login in the console when integrated
         * authentication is off.
         */
        $lock_account = 'unlock';
        $user_pass = '*';
    }

    if ($user_uid == 0) {
        $user_shell = isset($user['shell']) ? $user['shell'] : '/usr/local/sbin/opnsense-shell';
        $user_group = 'wheel';
        $user_home = '/root';
    } else {
        $user_shell = isset($user['shell']) ? $user['shell'] : '/sbin/nologin';
        $user_home = "/home/{$user_name}";
        $user_group = 'nobody';
    }

    /* admins access gives wheely rights */
    if (userIsAdmin($user['name'])) {
        $user_group = 'wheel';
    }
    // XXX: primary group id can only be wheel or nobody, otherwise we should map the correct numbers for comparison
    $user_gid = $user_group == 'wheel' ? 0 : 65534;

    if (!$force_password) {
        /* passwords only when integrated auth is disabled or forced */
        if (empty($config['system']['disableintegratedauth'])) {
            $user_pass = '*';
        }

        /* read from pw db if not provided (batch mode) */
        if ($userattrs === null) {
            $fd = popen("/usr/sbin/pw usershow -n {$user_name}", 'r');
            $pwread = fgets($fd);
            pclose($fd);
            if (substr_count($pwread, ':')) {
                $userattrs = explode(':', trim($pwread));
            }
        }
    }

    /* determine add or mod */
    if ($userattrs === null || $userattrs[0] != $user['name']) {
        $user_op = 'useradd -m -k /usr/share/skel -o';
    } elseif (
        $userattrs[0] == $user_name &&
        /* XXX $user_pass comparison? */
        $userattrs[2] == $user_uid &&
        $userattrs[3] == $user_gid &&
        $userattrs[7] == $comment &&
        $userattrs[8] == $user_home &&
        $userattrs[9] == $user_shell
    ) {
        // XXX: unchanged
        $user_op = null;
    } else {
        $user_op = 'usermod';
    }

    /* add or mod pw db */
    if ($user_op != null) {
        $cmd = "/usr/sbin/pw {$user_op} -q -u {$user_uid} -n {$user_name}" .
          " -g {$user_group} -s {$user_shell} -d {$user_home}" .
          " -c " . escapeshellarg($comment) . " -H 0 2>&1";
        $fd = popen($cmd, 'w');
        fwrite($fd, $user_pass);
        pclose($fd);
    }

    /* create user directory if required */
    @mkdir($user_home, 0700);
    @chown($user_home, $user_name);
    @chgrp($user_home, $user_group);

    /* write out ssh authorized key file */
    if ($is_unlocked && isset($user['authorizedkeys'])) {
        @mkdir("{$user_home}/.ssh", 0700);
        @chown("{$user_home}/.ssh", $user_name);
        $keys = base64_decode($user['authorizedkeys']);
        $keys = preg_split('/[\n\r]+/', $keys);
        $keys[] = '';
        $keys = implode("\n", $keys);
        @file_put_contents("{$user_home}/.ssh/authorized_keys", $keys);
        @chown("{$user_home}/.ssh/authorized_keys", $user_name);
    } else {
        @unlink("{$user_home}/.ssh/authorized_keys");
    }

    mwexecf('/usr/sbin/pw %s %s', array($lock_account, $user_name), true);
}

function local_user_del($user)
{
    /* remove all memberships */
    local_user_set_groups($user);

    /* delete from pw db */
    mwexecf('/usr/sbin/pw userdel -n %s -r', $user['name']);

    /* Delete user from groups needs a call to write_config() */
    local_group_del_user($user);
}

function local_user_set_password(&$user, $password = null)
{
    if ($password == null) {
        /* generate a random password */
        $password = random_bytes(50);
    }

    $hash = password_hash($password, PASSWORD_BCRYPT, [ 'cost' => 10 ]);
    if ($hash !== false) {
        $user['password'] = $hash;
    }
}

function local_user_get_groups($user)
{
    global $config;

    $groups = array();

    if (!isset($config['system']['group'])) {
        return $groups;
    }

    foreach ($config['system']['group'] as $group) {
        if (isset($group['member'])) {
            if (in_array($user['uid'], $group['member'])) {
                $groups[] = $group['name'];
            }
        }
    }

    sort($groups);

    return $groups;
}

function local_user_set_groups($user, $new_groups = null)
{
    global $config, $groupindex;

    if (!isset($config['system']['group'])) {
        return;
    }

    $cur_groups = local_user_get_groups($user);
    $mod_groups = array();

    if (!is_array($new_groups)) {
        $new_groups = array();
    }

    if (!is_array($cur_groups)) {
        $cur_groups = array();
    }

    /* determine which memberships to add */
    foreach ($new_groups as $groupname) {
        if (in_array($groupname, $cur_groups) || !isset($groupindex[$groupname])) {
            // continue if group is already in current list or the groupname is invalid
            continue;
        }
        $group = &config_read_array('system', 'group', $groupindex[$groupname]);
        $group['member'][] = $user['uid'];
        $mod_groups[] = $group;
    }

    /* determine which memberships to remove */
    foreach ($cur_groups as $groupname) {
        if (in_array($groupname, $new_groups)) {
            continue;
        }
        if (!isset($config['system']['group'][$groupindex[$groupname]])) {
            continue;
        }
        $group = &config_read_array('system', 'group', $groupindex[$groupname]);
        if (is_array($group['member'])) {
            $index = array_search($user['uid'], $group['member']);
            array_splice($group['member'], $index, 1);
            $mod_groups[] = $group;
        }
    }

    /* sync all modified groups */
    foreach ($mod_groups as $group) {
        local_group_set($group);
    }
}

function local_group_del_user($user)
{
    global $config;

    if (!isset($config['system']['group'])) {
        return;
    }

    foreach ($config['system']['group'] as $group) {
        if (isset($group['member'])) {
            foreach ($group['member'] as $idx => $uid) {
                if ($user['uid'] == $uid) {
                    unset($config['system']['group']['member'][$idx]);
                }
            }
        }
    }
}

function local_group_set($group)
{
    if (!isset($group['name']) || !isset($group['gid'])) {
        return;
    }

    $group_name = $group['name'];
    $group_gid = $group['gid'];
    $group_members = '';

    if (!empty($group['member']) && count($group['member']) > 0) {
        $group_members = implode(',', $group['member']);
    }

    $ret = mwexecf('/usr/sbin/pw groupshow %s', $group_name, true);
    if ($ret) {
        $group_op = 'groupadd';
    } else {
        $group_op = 'groupmod';
    }

    mwexecf('/usr/sbin/pw %s %s -g %s -M %s', array($group_op, $group_name, $group_gid, $group_members));
}

function local_group_del($group)
{
    /* delete from group db */
    mwexecf('/usr/sbin/pw groupdel %s', $group['name']);
}


/**
 * @param $name string name of the authentication system configured on the authentication server page or 'Local Database' for local authentication
 * @return array|bool false if the authentication server was not found, otherwise the configuration of the authentication server
 */
function auth_get_authserver($name)
{
    global $config;

    if ($name == "Local Database") {
        return array(
          "name" => gettext("Local Database"),
          "type" => "local",
          "host" => $config['system']['hostname']
        );
    }

    if (!empty($config['system']['authserver'])) {
        foreach ($config['system']['authserver'] as $authcfg) {
            if ($authcfg['name'] == $name) {
                if ($authcfg['type'] == 'ldap' || $authcfg['type'] == 'ldap-totp') {
                    // make sure a user and password entry exists and are null for anonymous usage
                    if (empty($authcfg['ldap_binddn'])) {
                        $authcfg['ldap_binddn'] = null;
                    }
                    if (empty($authcfg['ldap_bindpw'])) {
                        $authcfg['ldap_bindpw'] = null;
                    }
                }
                return $authcfg;
            }
        }
    }

    return false;
}

function auth_get_authserver_list()
{
    global $config;

    $list = array();

    if (!empty($config['system']['authserver'])) {
        foreach ($config['system']['authserver'] as $authcfg) {
            /* Add support for disabled entries? */
            $list[$authcfg['name']] = $authcfg;
        }
    }

    $list["Local Database"] = array( "name" => gettext("Local Database"), "type" => "local", "host" => $config['system']['hostname']);
    return $list;
}

/**
 * return authenticator object
 * @param array|null $authcfg configuration
 * @return Auth\Base type object
 */
function get_authenticator($authcfg = null)
{
    if (empty($authcfg)) {
        $authName = 'Local Database';
    } else {
        $authName = $authcfg['name'];
        if ($authcfg['type'] == 'local') {
            // avoid gettext type issues on Local Database, authenticator should always be named "Local Database"
            $authName = 'Local Database';
        }
    }

    $authFactory = new OPNsense\Auth\AuthenticationFactory();
    return $authFactory->get($authName);
}
