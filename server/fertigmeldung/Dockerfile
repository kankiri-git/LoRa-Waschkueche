FROM python:3.12-slim
RUN pip install --no-cache-dir paho-mqtt==2.1.0
WORKDIR /app
COPY fertig.py fertigmeldung.py ./
RUN useradd --system --uid 1000 melder && mkdir -p /data && chown melder /data
USER melder
ENV PYTHONUNBUFFERED=1 DATEN=/data
CMD ["python", "fertigmeldung.py"]
