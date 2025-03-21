<?php

/*
 * Copyright (C) 2020 Deciso B.V.
 * Copyright (C) 2018 Martin Wasley <martin@team-rebellion.net>
 * Copyright (C) 2016-2022 Franco Fichtner <franco@opnsense.org>
 * Copyright (C) 2008 Bill Marquette <bill.marquette@gmail.com>
 * Copyright (C) 2008 Seth Mos <seth.mos@dds.nl>
 * Copyright (C) 2010 Ermal Luçi
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
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

function dpinger_services()
{
    global $config;

    $services = array();

    if (!isset($config['gateways']['gateway_item'])) {
        return $services;
    }

    foreach ($config['gateways']['gateway_item'] as $gateway) {
        if (!empty($gateway['monitor_disable']) || isset($gateway['disabled'])) {
            continue;
        }

        if (empty($config['interfaces'][$gateway['interface']]['enable'])) {
            continue;
        }

        $pconfig = [];
        $pconfig['description'] = sprintf(gettext('Gateway Monitor (%s)'), $gateway['name']);
        $pconfig['php']['restart'] = ['dpinger_configure_do'];
        $pconfig['php']['start'] = ['dpinger_configure_do'];
        $pconfig['pidfile'] = "/var/run/dpinger_{$gateway['name']}.pid";
        $pconfig['php']['args'] = ['verbose', 'id'];
        $pconfig['name'] = 'dpinger';
        $pconfig['verbose'] = false;
        $pconfig['id'] = $gateway['name'];
        $services[] = $pconfig;
    }

    return $services;
}

function dpinger_defaults()
{
    return array(
        'alert_interval' => '1',
        'data_length' => '0',
        'interval' => '1',
        'latencyhigh' => '500',
        'latencylow' => '200',
        'loss_interval' => '2',
        'losshigh' => '20',
        'losslow' => '10',
        'time_period' => '60',
    );
}

function dpinger_configure()
{
    return [
        'monitor' => ['dpinger_configure_do:3'],
    ];
}

function dpinger_syslog()
{
    $logfacilities = [];

    $logfacilities['gateways'] = ['facility' => ['dpinger']];

    return $logfacilities;
}

function dpinger_configure_do($verbose = false, $gwname = null, $bootup = false)
{
    service_log(sprintf('Setting up gateway monitor%s...', empty($gwname) ? 's' : " {$gwname}"), $verbose);

    foreach (dpinger_processes() as $running_gwname => $process) {
        if (!empty($gwname) && $running_gwname != $gwname) {
            continue;
        }
        killbypid($process['pidfile'], 'TERM', true);
        @unlink($process['pidfile']);
        @unlink($process['socket']);
    }

    $ifconfig_details = legacy_interfaces_details();
    $gateways = new \OPNsense\Routing\Gateways($ifconfig_details);
    $monitor_ips = [];

    $dpinger_default = dpinger_defaults();
    foreach ($gateways->gatewaysIndexedByName(true) as $name => $gateway) {
        if (!empty($gwname) && $gwname != $name) {
            continue;
        } elseif (empty($gateway['monitor'])) {
            log_msg(sprintf('The %s monitor address is empty, skipping.', $name), LOG_WARNING);
            continue;
        } elseif (in_array($gateway['monitor'], $monitor_ips)) {
            log_msg(sprintf('The %s monitor address "%s" cannot be used twice, skipping.', $name, $gateway['monitor']), LOG_WARNING);
            continue;
        }

        /*
         * Interface IP is needed since dpinger will bind a socket to it.
         * However the config GUI should already have checked this and when
         * PPPoE is used the IP address is set to "dynamic".  So using
         * is_ipaddrv4() or is_ipaddrv6() to identify packet type would be
         * wrong, especially as further checks (that can cope with the
         * "dynamic" case) are present inside the if block.  So using
         * $gateway['ipprotocol'] is the better option.
         */
        if ($gateway['ipprotocol'] == "inet") { // This is an IPv4 gateway...
            $gwifip = null;

            if (is_ipaddrv4($gateway['gateway'])) {
                foreach (interfaces_addresses($gateway['interface'], false, $ifconfig_details) as $addr) {
                    /* explicitly do not require $addr['alias'] to be true here */
                    if ($addr['family'] != 'inet') {
                        continue;
                    }

                    $network = gen_subnet($addr['address'], $addr['bits']) . "/{$addr['bits']}";

                    if (ip_in_subnet($gateway['gateway'], $network)) {
                        $gwifip = $addr['address'];
                        break;
                    }
                }
            }

            if ($gwifip == null) {
                list ($gwifip) = interfaces_primary_address($gateway['interface'], $ifconfig_details);
                if (!empty($gwifip) && is_ipaddrv4($gateway['gateway'])) {
                    log_msg(sprintf('Chose to bind %s on %s since we could not find a proper match.', $name, $gwifip));
                }
            }

            if (empty($gwifip)) {
                log_msg(sprintf('The required %s IPv4 interface address could not be found, skipping.', $name), LOG_WARNING);
                continue;
            }

            /* flush the monitor unconditionally */
            if (is_ipaddrv4($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_msg("Removing static route for monitor {$gateway['monitor']} via {$gateway['gateway']}", LOG_INFO);
                system_host_route($gateway['monitor'], $gateway['gateway'], true, false);
            }

            /* Do not monitor if such was requested */
            if (isset($gateway['disabled']) || isset($gateway['monitor_disable'])) {
                continue;
            }

            /*
             * If the gateway is the same as the monitor we do not add a
             * route as this will break the routing table.
             * Add static routes for each gateway with their monitor IP
             * not strictly necessary but is an added level of protection.
             */
            if (is_ipaddrv4($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_msg("Adding static route for monitor {$gateway['monitor']} via {$gateway['gateway']}", LOG_INFO);
                system_host_route($gateway['monitor'], $gateway['gateway'], false, true);
            }
        } elseif ($gateway['ipprotocol'] == "inet6") { // This is an IPv6 gateway...
            $gwifip = null;

            if (is_linklocal($gateway['monitor'])) {
                /* link local monitor needs a link local address for the "src" part */
                list ($gwifip) = interfaces_scoped_address6($gateway['interface'], $ifconfig_details);
            } else {
                list ($gwifip) = interfaces_primary_address6($gateway['interface'], $ifconfig_details);
            }

            if ($gwifip == null && is_ipaddrv6($gateway['gateway'])) {
                foreach (interfaces_addresses($gateway['interface'], false, $ifconfig_details) as $addr) {
                    if ($addr['family'] != 'inet6' || !$addr['alias']) {
                        continue;
                    }

                    $networkv6 = gen_subnetv6($addr['address'], $addr['bits']) . "/{$addr['bits']}";

                    if (ip_in_subnet($gateway['gateway'], $networkv6)) {
                        $gwifip = $addr['address'];
                        break;
                    }
                }
            }

            if (empty($gwifip)) {
                log_msg(sprintf('The required %s IPv6 interface address could not be found, skipping.', $name), LOG_WARNING);
                continue;
            }

            /*
             * If gateway is a local link and 'monitor' is global routable
             * then the ICMP6 response would not find its way back home.
             */
            if (is_linklocal($gateway['monitor']) && strpos($gateway['monitor'], '%') === false) {
                $gateway['monitor'] .= '%' . get_real_interface($gateway['interface'], 'inet6');
            }
            if (is_linklocal($gateway['gateway']) && strpos($gateway['gateway'], '%') === false) {
                $gateway['gateway'] .= '%' . get_real_interface($gateway['interface'], 'inet6');
            }

            /* flush the monitor unconditionally */
            if (is_ipaddrv6($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_msg("Removing static route for monitor {$gateway['monitor']} via {$gateway['gateway']}", LOG_INFO);
                system_host_route($gateway['monitor'], $gateway['gateway'], true, false);
            }

            /* Do not monitor if such was requested */
            if (isset($gateway['disabled']) || isset($gateway['monitor_disable'])) {
                continue;
            }

            /*
             * If the gateway is the same as the monitor we do not add a
             * route as this will break the routing table.
             * Add static routes for each gateway with their monitor IP
             * not strictly necessary but is an added level of protection.
             */
            if (is_ipaddrv6($gateway['gateway']) && $gateway['monitor'] != $gateway['gateway']) {
                log_msg("Adding static route for monitor {$gateway['monitor']} via {$gateway['gateway']}", LOG_INFO);
                system_host_route($gateway['monitor'], $gateway['gateway'], false, true);
            }
        } else {
            log_msg(sprintf('Unknown address family "%s" during monitor setup', $gateway['ipprotocol']), LOG_ERR);
            continue;
        }

        /*
         * Create custom RRD graph with suitable settings that
         * may differ from the daemon's standards.
         */
        require_once("rrd.inc");
        rrd_create_gateway_quality("/var/db/rrd/{$gateway['name']}-quality.rrd", $bootup);

        /* log warnings via syslog */
        $params  = '-S ';

        /* disable unused reporting thread */
        $params .= '-r 0 ';

        /* identifier */
        $params .= exec_safe('-i %s ', $name);

        /* bind src address */
        $params .= exec_safe('-B %s ', $gwifip);

        /* PID filename */
        $params .= exec_safe('-p %s ', "/var/run/dpinger_{$name}.pid");

        /* status socket */
        $params .= exec_safe('-u %s ', "/var/run/dpinger_{$name}.sock");

        /* command to run on alarm */
        $params .= '-C "/usr/local/etc/rc.syshook monitor" ';

        foreach (
            [
            'interval' => '-s %ss ',
            'loss_interval' => '-l %ss ',
            'time_period' => '-t %ss ',
            'alert_interval' => '-A %ss ',
            'latencyhigh' => '-D %s ',
            'losshigh' => '-L %s ',
            'data_length' => '-d %s '
            ] as $pname => $ppattern
        ) {
            $params .= exec_safe(
                $ppattern,
                isset($gateway[$pname]) && is_numeric($gateway[$pname]) ?
                    $gateway[$pname] : $dpinger_default[$pname]
            );
        }
        $params .= exec_safe('%s ', $gateway['monitor']);

        /* foreground mode in background to deal with tentative connectivity */
        mwexec_bg("/usr/local/bin/dpinger -f {$params}");
    }

    service_log("done.\n", $verbose);
}

function dpinger_run()
{
    return array(
        'return_gateways_status' => 'dpinger_status',
    );
}

function dpinger_status()
{
    $status = array();
    $gateways_arr = array();

    foreach (config_read_array('gateways', 'gateway_item') as $gwitem) {
        if (isset($gwitem['disabled'])) {
            continue;
        }
        $gateways_arr[$gwitem['name']] = $gwitem;

        $gwstatus = isset($gwitem['monitor_disable']) ? 'none' : 'down';

        if (isset($gwitem['force_down'])) {
            $gwstatus = 'force_down';
        }

        $status[$gwitem['name']] = array(
            'name' => $gwitem['name'],
            'status' => $gwstatus,
            'stddev' => '~',
            'delay' => '~',
            'loss' => '~',
        );
    }

    foreach (dpinger_processes() as $gwname => $proc) {
        if (!isset($status[$gwname])) {
            continue;
        }

        $fp = @stream_socket_client("unix://{$proc['socket']}", $errno, $errstr, 3);
        if (!$fp) {
            continue;
        }

        $dinfo = '';
        while (!feof($fp)) {
            $dinfo .= fgets($fp, 1024);
        }

        fclose($fp);

        $r = array();

        list($r['gwname'], $r['latency_avg'], $r['latency_stddev'], $r['loss']) =
            explode(' ', preg_replace('/\n/', '', $dinfo));

        if ($r['latency_stddev'] == '0' && $r['loss'] == '0') {
            /* not yet ready, act like nothing was returned, but don't consider the gateway to be down */
            $status[$gwname] = array(
                'name' => $gwname,
                'status' => 'none',
                'stddev' => '~',
                'delay' => '~',
                'loss' => '~',
            );
        } else {
            $r['latency_stddev'] = round($r['latency_stddev'] / 1000, 1);
            $r['latency_avg'] = round($r['latency_avg'] / 1000, 1);
            $r['status'] = $status[$gwname]['status'];

            $settings = dpinger_defaults();

            if ($r['status'] != 'force_down') {
                $keys = array('latencylow', 'latencyhigh', 'losslow', 'losshigh');

                /* Replace default values by user-defined */
                foreach ($keys as $key) {
                    if (isset($gateways_arr[$gwname][$key]) && is_numeric($gateways_arr[$gwname][$key])) {
                        $settings[$key] = $gateways_arr[$gwname][$key];
                    }
                }

                if ($r['latency_avg'] > $settings['latencyhigh']) {
                    $r['status'] = 'down';
                } elseif ($r['loss'] > $settings['losshigh']) {
                    $r['status'] = 'down';
                } elseif ($r['latency_avg'] > $settings['latencylow']) {
                    $r['status'] = 'delay';
                } elseif ($r['loss'] > $settings['losslow']) {
                    $r['status'] = 'loss';
                } else {
                    $r['status'] = 'none';
                }
            }

            $status[$gwname] = array(
                'delay' => sprintf('%0.1f ms', empty($r['latency_avg']) ? 0.0 : round($r['latency_avg'], 1)),
                'stddev' => sprintf('%0.1f ms', empty($r['latency_stddev']) ? 0.0 : round($r['latency_stddev'], 1)),
                'loss' => sprintf('%0.1f %%', empty($r['loss']) ? 0.0 : round($r['loss'], 1)),
                'status' => $r['status'],
                'details' => array_merge($r, $settings),
                'name' => $gwname,
            );
        }
    }

    return $status;
}

function dpinger_processes()
{
    $result = array();

    $pidfiles = glob('/var/run/dpinger_*.pid');
    if ($pidfiles === false) {
        return $result;
    }

    foreach ($pidfiles as $pidfile) {
        if (!isvalidpid($pidfile)) {
            /* spare caller from trying to read a stale socket later on */
            continue;
        }
        if (preg_match('/^dpinger_(.+)\.pid$/', basename($pidfile), $matches)) {
            $socket_file = preg_replace('/\.pid$/', '.sock', $pidfile);
            $result[$matches[1]] = array('socket' => $socket_file, 'pidfile' => $pidfile);
        }
    }

    return $result;
}
