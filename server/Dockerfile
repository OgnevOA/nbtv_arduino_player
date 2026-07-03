FROM python:3.12-slim

# ffmpeg for decode/scale; the rest is pip. Single image: bot+encoder+stream.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /srv

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

ENV HTTP_HOST=0.0.0.0 \
    HTTP_PORT=32125 \
    CACHE_DIR=/data/cache \
    DEFAULT_SPEED=0.95 \
    DEFAULT_MAX_HEIGHT=360

VOLUME ["/data"]
EXPOSE 32125

CMD ["python", "-m", "app"]
