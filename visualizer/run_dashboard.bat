@echo off
REM Start the QuantFusion Dashboard (Windows)

echo.
echo 🚀 Starting QuantFusion Dashboard...
echo ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
echo Dashboard will open at: http://localhost:8501
echo Press Ctrl+C to stop
echo ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
echo.

streamlit run dashboard.py
