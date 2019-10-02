import os
import sys
import random
import subprocess
import time
import turicreate as tc

####### Training 
# Run image_classif_create.py first

tc.config.set_runtime_config('TURI_DEFAULT_NUM_PYLAMBDA_WORKERS', 32)

pid=os.getpid()
print "My pid is " + str(pid)

# Load the data
train_data = tc.SFrame('cats-dogs-train.sframe')
test_data = tc.SFrame('cats-dogs-test.sframe')

# Create the model
model = tc.image_classifier.create(train_data, target='label')

# Save the model for later use in Turi Create
model.save('mymodel.model')

sys.stdout.flush()
