//go:build windows

package job

import (
	"os/exec"
	"strconv"
	"syscall"
)

// createNewProcessGroup keeps the CLI in its own group so taskkill /T can
// reach every child the remesher may spawn.
const createNewProcessGroup = 0x00000200

func setProcAttr(cmd *exec.Cmd) {
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	cmd.SysProcAttr.CreationFlags |= createNewProcessGroup
}

// killTree terminates the process and its descendants. taskkill /T /F is the
// reliable Windows path; Process.Kill is the fallback when taskkill is absent.
func killTree(cmd *exec.Cmd) error {
	if cmd == nil || cmd.Process == nil {
		return nil
	}
	pid := strconv.Itoa(cmd.Process.Pid)
	if err := exec.Command("taskkill", "/T", "/F", "/PID", pid).Run(); err == nil {
		return nil
	}
	return cmd.Process.Kill()
}
