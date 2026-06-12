"""
QuantFusion Visualizer — Quick Setup Script
Installs dependencies and validates the environment
"""

import sys
import subprocess
from pathlib import Path

def run_command(cmd, description=""):
    """Run a shell command and report status"""
    if description:
        print(f"\n  {description}...")
    
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, check=True)
        print(f"  ✓ Done")
        return True
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Failed: {e.stderr}")
        return False

def main():
    print("\n" + "="*70)
    print("QuantFusion Visualizer — Setup & Installation")
    print("="*70 + "\n")
    
    visualizer_dir = Path(__file__).parent
    
    # Check Python version
    print("📋 Checking Python environment...")
    print(f"   Python version: {sys.version}")
    print(f"   Python path: {sys.executable}")
    
    if sys.version_info < (3, 8):
        print("   ✗ Python 3.8+ required")
        sys.exit(1)
    print("   ✓ Python version OK")
    
    # Install dependencies
    print("\n📦 Installing dependencies...")
    requirements_file = visualizer_dir / "requirements.txt"
    
    if requirements_file.exists():
        cmd = f"{sys.executable} -m pip install -r {requirements_file}"
        if run_command(cmd, "Installing packages from requirements.txt"):
            print("   ✓ All dependencies installed")
        else:
            print("   ✗ Failed to install dependencies")
            sys.exit(1)
    else:
        print(f"   ✗ requirements.txt not found at {requirements_file}")
        sys.exit(1)
    
    # Verify installation
    print("\n🔍 Verifying installation...")
    required_packages = ["streamlit", "plotly", "pandas", "numpy"]
    all_ok = True
    
    for package in required_packages:
        try:
            __import__(package)
            print(f"   ✓ {package} installed")
        except ImportError:
            print(f"   ✗ {package} not found")
            all_ok = False
    
    if not all_ok:
        print("\n   ✗ Some packages are missing. Try running:")
        print(f"   {sys.executable} -m pip install -r {requirements_file}")
        sys.exit(1)
    
    # Create directories
    print("\n📁 Creating data directories...")
    research_db = visualizer_dir.parent / "research_db"
    for subdir in ["nav_curves", "trades", "alphas", "strategies"]:
        (research_db / subdir).mkdir(parents=True, exist_ok=True)
        print(f"   ✓ {subdir}/")
    
    # Run example
    print("\n🎬 Running example export...")
    example_script = visualizer_dir / "example_export.py"
    if example_script.exists():
        cmd = f"{sys.executable} {example_script}"
        run_command(cmd, "Generating sample data")
    
    # Print next steps
    print("\n" + "="*70)
    print("✨ Setup Complete!")
    print("="*70)
    print("\n🚀 Next steps:")
    print("\n1. Run the dashboard:")
    print(f"   cd {visualizer_dir}")
    print("   streamlit run dashboard.py")
    print("\n2. Open in browser: http://localhost:8501")
    print("\n3. To export your own backtest results, use:")
    print("   from visualizer.data_export import QuantFusionDataExporter")
    print("   exporter = QuantFusionDataExporter()")
    print("   exporter.export_nav_curve(...)")
    print("\n" + "="*70 + "\n")

if __name__ == "__main__":
    main()
