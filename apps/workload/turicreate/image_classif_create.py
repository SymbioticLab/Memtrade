import os
import sys
import random
import subprocess
import time
import turicreate as tc

###### Create train and test data 
# From https://apple.github.io/turicreate/docs/userguide/image_classifier/
# Download dataset from: https://www.microsoft.com/en-us/download/details.aspx?id=54765 (large dataset)
# and change home to your dataset

######################

home='./datasets/kagglecatsanddogs_3367a/PetImages'

######################

tc.config.set_runtime_config('TURI_DEFAULT_NUM_PYLAMBDA_WORKERS', 32)

# Load images (Note: you can ignore 'Not a JPEG file' errors)
data = tc.image_analysis.load_images(home, with_path=True)

# From the path-name, create a label column
data['label'] = data['path'].apply(lambda path: 'dog' if '/Dog' in path else 'cat')

# Save the data for future use
data.save('cats-dogs.sframe')

# Load the data
data =  tc.SFrame('cats-dogs.sframe')

# Make a train-test split
train_data, test_data = data.random_split(0.8)

train_data.save('cats-dogs-train.sframe')
test_data.save('cats-dogs-test.sframe')

sys.stdout.flush()
