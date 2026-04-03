FROM python:3.12-slim

WORKDIR /app

# Install dependencies
# Note: build context is esp32-pm-monitor/ when running from that directory
COPY backend/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy application
COPY backend/app.py .
COPY frontend ./frontend

# Create directory for SQLite database
RUN mkdir -p /data

ENV DB_PATH=/data/pm_data.db

EXPOSE 8000

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
