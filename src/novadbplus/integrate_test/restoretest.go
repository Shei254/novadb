// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

package main

import (
	"flag"
	"integrate_test/util"
	"math"
	"strconv"
	"strings"
	"time"

	"github.com/ngaut/log"
)

func testRestore(m1_ip string, m1_port int, m2_ip string, m2_port int, kvstorecount int, backup_mode string) {
	m1 := util.RedisServer{}
	m2 := util.RedisServer{}
	pwd := util.GetCurrentDirectory()
	log.Infof("current pwd:" + pwd)

	cfgArgs := make(map[string]string)
	cfgArgs["maxBinlogKeepNum"] = "1"
	cfgArgs["kvstorecount"] = strconv.Itoa(kvstorecount)
	cfgArgs["requirepass"] = "novadb+test"
	cfgArgs["masterauth"] = "novadb+test"
	cfgArgs["truncateBinlogNum"] = "1"

	m1_port = util.FindAvailablePort(m1_port)
	log.Infof("FindAvailablePort:%d", m1_port)
	m1.Init(m1_ip, m1_port, pwd, "m1_", util.Standalone)
	if err := m1.Setup(false, &cfgArgs); err != nil {
		log.Fatalf("setup master1 failed:%v", err)
	}
	m2_port = util.FindAvailablePort(m2_port)
	log.Infof("FindAvailablePort:%d", m2_port)
	m2.Init(m2_ip, m2_port, pwd, "s1_", util.Standalone)
	if err := m2.Setup(false, &cfgArgs); err != nil {
		log.Fatalf("setup master2 failed:%v", err)
	}
	time.Sleep(15 * time.Second)

	// check path cant equal dbPath
	cli := createClient(&m1)
	if r, err := cli.Cmd("backup", m1.Path+"/db", backup_mode).Str(); err.Error() != ("ERR:4,msg:dir cant be dbPath:" + m1.Path + "/db") {
		log.Fatalf("backup dir cant be dbPath:%v %s", err, r)
		return
	}
	// check path must exist
	if r, err := cli.Cmd("backup", "dir_not_exist", backup_mode).Str(); err.Error() != ("ERR:4,msg:dir not exist:dir_not_exist") &&
		!strings.Contains(err.Error(), "No such file or directory") {
		log.Fatalf("backup dir must exist:%v %s", err, r)
		return
	}

	ch := make(chan int)
	util.AddData(&m1, *auth, *num1, 0, 0, "aa", ch)
	<-ch
	backup(&m1, backup_mode, "/tmp/back_test")
	restoreBackup(&m2, "/tmp/back_test")

	util.AddData(&m1, *auth, *num2, 0, 0, "bb", ch)
	<-ch
	util.AddOnekeyEveryStore(&m1, *auth, kvstorecount)
	waitDumpBinlog(&m1, kvstorecount)
	flushBinlog(&m1)
	restoreBinlog(&m1, &m2, kvstorecount, math.MaxUint64)
	util.AddOnekeyEveryStore(&m2, *auth, kvstorecount)
	compare(&m1, &m2)

	shutdownServer(&m1, *shutdown, *clear)
	shutdownServer(&m2, *shutdown, *clear)
}

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds | log.Lshortfile)
	flag.Parse()
	// rand.Seed(time.Now().UnixNano())
	testRestore(*m1ip, *m1port, *m2ip, *m2port, *kvstorecount, "copy")
	// port+100 to avoid TIME_WAIT
	testRestore(*m1ip, *m1port+100, *m2ip, *m2port+100, *kvstorecount, "ckpt")
	log.Infof("restoretest.go passed.")
}
