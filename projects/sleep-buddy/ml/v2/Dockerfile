FROM tensorflow/tensorflow:2.20.0

COPY requirements.txt /tmp/requirements.txt
RUN grep -v '^tensorflow==' /tmp/requirements.txt > /tmp/container-requirements.txt \
    && pip install --no-cache-dir -r /tmp/container-requirements.txt

WORKDIR /workspace
