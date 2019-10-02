import os
import sys
import random
import subprocess
import time
import turicreate as tc

tc.config.set_runtime_config('TURI_DEFAULT_NUM_PYLAMBDA_WORKERS', 32)

pid=os.getpid()
print "My pid is " + str(pid)

# Load the test data
#data =  tc.SFrame('cats-dogs.sframe')
test_data = tc.SFrame('cats-dogs-test.sframe')

# Load the model
model = tc.load_model('mymodel.model')
time.sleep(5)

print "Running prediction"
sys.stdout.flush()

#do_attach_pbsim()

start=time.time()
predictions = model.predict(test_data)
end=time.time()
print "Prediction finished"
print end - start
sys.stdout.flush()
