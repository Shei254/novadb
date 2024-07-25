// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

package main

import (
	"flag"
	"integrate_test/util"
	"strconv"
	"time"

	"github.com/ngaut/log"
)

func waitFullsyncAndCatchup(m *util.RedisServer, s *util.RedisServer, kvstorecount int, channel chan int) {
	waitFullsync(s, kvstorecount)
	waitCatchup(m, s, kvstorecount)
	channel <- 0
}

func testRepl(m1_ip string, m1_port int, s1_ip string, s1_port int, kvstorecount int) {
	m1 := util.RedisServer{}
	s1 := util.RedisServer{}
	pwd := util.GetCurrentDirectory()
	log.Infof("current pwd:" + pwd)
	num1 := 100000
	num2 := 100000
	if *startup == 1 {
		cfgArgs := make(map[string]string)
		cfgArgs["maxBinlogKeepNum"] = strconv.Itoa(num2 * 5)
		//cfgArgs["maxBinlogKeepNum"] = strconv.Itoa(1)
		cfgArgs["kvstorecount"] = strconv.Itoa(kvstorecount)
		cfgArgs["rocks.blockcachemb"] = strconv.Itoa(1024)
		cfgArgs["requirepass"] = "novadb+test"
		cfgArgs["direct-io"] = "true"

		m1_port = util.FindAvailablePort(m1_port)
		log.Infof("FindAvailablePort:%d", m1_port)
		m1.Init(m1_ip, m1_port, pwd, "m1_", util.Standalone)
		if err := m1.Setup(false, &cfgArgs); err != nil {
			log.Fatalf("setup master1 failed:%v", err)
		}
		s1_port = util.FindAvailablePort(s1_port)
		log.Infof("FindAvailablePort:%d", s1_port)
		s1.Init(s1_ip, s1_port, pwd, "s1_", util.Standalone)
		cfgArgs["masterauth"] = "novadb+test"
		if err := s1.Setup(false, &cfgArgs); err != nil {
			log.Fatalf("setup slave1 failed:%v", err)
		}
	}
	time.Sleep(15 * time.Second)

	util.AddDataWithBenchmark(&m1, *auth, num1, *keyprefix1, "set")
	time.Sleep(1 * time.Second)
	slaveof(&m1, &s1)
	//waitFullsync(&s1, kvstorecount)
	//waitCatchup(&m1, &s1, kvstorecount)

	var channel chan int = make(chan int)
	go waitFullsyncAndCatchup(&m1, &s1, kvstorecount, channel)
	util.AddData(&m1, *auth, num2, 0, 0, *keyprefix2, channel)
	<-channel
	<-channel

	waitCatchup(&m1, &s1, kvstorecount)
	if *iscompare == 1 {
		compare(&m1, &s1)
	}

	shutdownServer(&m1, *shutdown, *clear)
	shutdownServer(&s1, *shutdown, *clear)
}

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds | log.Lshortfile)
	flag.Parse()
	// rand.Seed(time.Now().UnixNano())
	testRepl(*m1ip, *m1port, *s1ip, *s1port, *kvstorecount)
	log.Infof("repltest.go passed.")
}
