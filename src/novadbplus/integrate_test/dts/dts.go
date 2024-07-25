package main

import (
	"bytes"
	"flag"
	"integrate_test/util"
	"math/rand"
	"os/exec"
	"time"

	"github.com/ngaut/log"
)

var (
	mport = flag.Int("masterport", 62001, "master port")
	sport = flag.Int("slaveport", 62002, "slave port")
	tport = flag.Int("targetport", 62003, "target port")
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds | log.Lshortfile)
	flag.Parse()
	rand.Seed(time.Now().UnixNano())

	cfgArgs := make(map[string]string)
	cfgArgs["aof-enabled"] = "yes"
	cfgArgs["kvStoreCount"] = "1"
	cfgArgs["noexpire"] = "false"
	cfgArgs["generallog"] = "true"
	pwd := util.GetCurrentDirectory()

	m := new(util.RedisServer)
	m.WithBinPath("novadbplus")
	m.Ip = "127.0.0.1"
	masterPort := util.FindAvailablePort(*mport)
	log.Infof("FindAvailablePort:%d", masterPort)

	m.Init("127.0.0.1", masterPort, pwd, "m_", util.Standalone)

	if err := m.Setup(false, &cfgArgs); err != nil {
		log.Fatalf("setup master failed:%v", err)
	}
	defer util.ShutdownServer(m)

	s := new(util.RedisServer)
	s.Ip = "127.0.0.1"
	s.WithBinPath("novadbplus")
	slavePort := util.FindAvailablePort(*sport)
	log.Infof("FindAvailablePort:%d", slavePort)

	s.Init("127.0.0.1", slavePort, pwd, "s_", util.Standalone)

	if err := s.Setup(false, &cfgArgs); err != nil {
		log.Fatalf("setup slave failed:%v", err)
	}
	defer util.ShutdownServer(s)

	util.SlaveOf(m, s)

	t := new(util.RedisServer)
	t.Ip = "127.0.0.1"
	t.WithBinPath("novadbplus")
	targetPort := util.FindAvailablePort(*tport)
	log.Infof("FindAvailablePort:%d", targetPort)

	t.Init("127.0.0.1", targetPort, pwd, "t_", util.Standalone)

	cfgArgs["aof-enabled"] = "false"
	cfgArgs["noexpire"] = "yes"
	if err := t.Setup(false, &cfgArgs); err != nil {
		log.Fatalf("setup target failed:%v", err)
	}
	defer util.ShutdownServer(t)

	time.Sleep(15 * time.Second)

	util.AddSomeData(m, "", 0)
	SpecifHashData(m, "", "first")

	var stdoutDTS bytes.Buffer
	var stderrDTS bytes.Buffer
	cmdDTS := exec.Command("checkdts", m.Addr(), "", t.Addr(), "", "0", "1", "8000", "0", "0")
	cmdDTS.Stdout = &stdoutDTS
	cmdDTS.Stderr = &stderrDTS
	err := cmdDTS.Run()
	if err != nil {
		log.Fatal(err)
	}
	//defer cmdDTS.Process.Kill()

	util.AddSomeData(m, "", 20000)
	SpecifHashData(m, "", "second")

	log.Info(stdoutDTS.String())
	log.Info(stderrDTS.String())

	// wait slave catch up master
	time.Sleep(time.Second * 10)
	// s and t no expire now
	util.CompareData(s.Addr(), t.Addr(), 1)

	util.ConfigSet(t, "noexpire", "false")
	time.Sleep(time.Second * 5)
	// still no expire
	util.CompareData(t.Addr(), s.Addr(), 1)

	// wait for expire
	// see specifHashData for more info.
	time.Sleep(120 * time.Second)
	// after expire, m s t should have same data.
	util.CompareData(m.Addr(), t.Addr(), 1)

	util.CompareData(m.Addr(), s.Addr(), 1)

	log.Infof("dts.go passed.")

}
