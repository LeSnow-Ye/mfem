#!/usr/bin/env python

# This is the regression testing script for ex5-nguygen
# The reference cases are stored in the regress_test folder

import os
import subprocess

class bcolors:
	HEADER = '\033[95m'
	OKGREEN = '\033[92m'
	FAIL = '\033[91m'
	RESET = "\033[0m"

tol = 1e-7

print('Running Regression Testing:')
path = 'regress_test/'
filenames = os.listdir(path)
failed = 0

for i in range(len(filenames)):
	# Parsing reference file

	filename = path + filenames[i]

	def get_ref_option(file, option):
		ref_out = subprocess.getoutput("grep ' "+option+"' "+file)
		return len(ref_out) > 0

	dg = get_ref_option(filename, '--discontinuous')
	hb = get_ref_option(filename, '--hybridization')
	upwind = get_ref_option(filename, '--upwinded')
	nonlin = get_ref_option(filename, '--nonlinear')
	nonlin_conv = get_ref_option(filename, '--nonlinear-convection')

	def get_ref_param(file, param, default=""):
		ref_out = subprocess.getoutput("grep ' "+param+"' "+file+"| cut -d ' ' -f 5")
		if len(ref_out) > 0:
			return ref_out.split()[0]
		else:
			return default

	problem = get_ref_param(filename, '--problem')
	order = get_ref_param(filename, '--order')
	nx = get_ref_param(filename, '--ncells-x')
	ny = get_ref_param(filename, '--ncells-y')
	kappa = get_ref_param(filename, '--kappa', "1")
	hdg = get_ref_param(filename, '--hdg_scheme', "1")

	ref_out = subprocess.getoutput("grep '|| t_h - t_ex || / || t_ex || = ' "+filename+"  | cut -d '=' -f 2-")
	ref_L2_t = float(ref_out.split()[0])
	ref_out = subprocess.getoutput("grep '|| q_h - q_ex || / || q_ex || = ' "+filename+"  | cut -d '=' -f 2-")
	ref_L2_q = float(ref_out.split()[0])
	if nonlin:
		ref_out = subprocess.getoutput("grep 'LBFGS+' "+filename+"  | cut -d '+' -f 2-")
	else:
		ref_out = subprocess.getoutput("grep 'GMRES+' "+filename+"  | cut -d '+' -f 2-")
	precond_ref = ref_out.split()[0]

	# Run test case
	print("----------------------------------------------------------------")
	print("Case: "+filenames[i])
	command_line = "./ex5-nguyen -no-vis -nx "+str(nx)+" -ny "+str(ny)+" -p "+problem+" -o "+order
	if dg:
		command_line = command_line+' -dg'
	if hb:
		command_line = command_line+' -hb'
	if upwind:
		command_line = command_line+' -up'
	if nonlin:
		command_line = command_line+' -nl'
	if nonlin_conv:
		command_line = command_line+' -nlc'
	if kappa != str(1):
		command_line = command_line+' -k '+kappa
	if hdg != str(1):
		command_line = command_line+' -hdg '+hdg

	cmd_out = subprocess.getoutput(command_line)
	split_cmd_out = cmd_out.splitlines()
	indx_t = split_cmd_out[-1].find('= ')
	indx_q = split_cmd_out[-2].find('= ')
	precond_test_idx_s = split_cmd_out[-4].find('+')
	precond_test_idx_e = split_cmd_out[-4].find(' ')

	test_L2_t = float(split_cmd_out[-1][indx_t+2::])
	test_L2_q = float(split_cmd_out[-2][indx_q+2::])
	precond_test = split_cmd_out[-4][precond_test_idx_s+1:precond_test_idx_e]

	if precond_test == precond_ref:
		if abs(ref_L2_t - test_L2_t) < tol and abs(ref_L2_q - test_L2_q) < tol:
			print(bcolors.OKGREEN + "SUCCESS: " + bcolors.RESET, end="", flush=True)
			print(command_line)
		else:
			print(bcolors.FAIL + "FAIL: " + bcolors.RESET, end="", flush=True)
			print(command_line)
			print(cmd_out)
			failed += 1
	else:
		print(bcolors.HEADER + "SKIPPING: "+ bcolors.RESET +command_line+" → incompatible preconditioner")

print("----------------------------------------------------------------")
if failed == 0:
	print(bcolors.OKGREEN + "SUCCESS: " + bcolors.RESET + "all tests finished succesfully!")
else:
	print(bcolors.FAIL + "FAIL: " + bcolors.RESET + str(failed) + " / " + str(len(filenames)) + " tests failed!")
