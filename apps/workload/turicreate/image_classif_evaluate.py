import os
import sys
import random
import subprocess
import time
import turicreate as tc

#### Evaluate the model 
# First create test and train data: image_classif_create.py
# And train the model: image_classif_model.py


tc.config.set_runtime_config('TURI_DEFAULT_NUM_PYLAMBDA_WORKERS', 32)

pid=os.getpid()
print "My pid is " + str(pid)

# Load the data
#data =  tc.SFrame('cats-dogs.sframe')
test_data = tc.SFrame('cats-dogs-test.sframe')

model = tc.load_model('mymodel.model')

# Evaluate the model and print the results
print "Evaluate model"
start=time.time()
metrics = model.evaluate(test_data)
end=time.time()
print end - start
print(metrics['accuracy'])
sys.stdout.flush()

