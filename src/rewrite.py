import tkinter as tk
from tkinter import filedialog
import re
import shutil

def replace_serial_prints(file_path):
    # Backup original file
    backup_path = file_path + ".bak"
    shutil.copyfile(file_path, backup_path)
    print(f"Backup created: {backup_path}")

    # Read the file content
    with open(file_path, 'r', encoding='utf-8') as file:
        content = file.read()

    # Replace Serial.println() and Serial.print()
    content = re.sub(r'\bSerial\.println\s*\(', 'DEBUG_PRINTLN(', content)
    content = re.sub(r'\bSerial\.print\s*\(', 'DEBUG_PRINT(', content)

    # Write the modified content back
    with open(file_path, 'w', encoding='utf-8') as file:
        file.write(content)
    print(f"File updated: {file_path}")

def main():
    root = tk.Tk()
    root.withdraw()  # Hide the main window

    file_path = filedialog.askopenfilename(
        title="Select a .cpp or .h file",
        filetypes=[("C++ Files", "*.cpp *.h"), ("All Files", "*.*")]
    )

    if file_path:
        replace_serial_prints(file_path)
    else:
        print("No file selected.")

if __name__ == "__main__":
    main()
