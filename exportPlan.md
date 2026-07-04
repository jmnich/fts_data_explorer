# General description of new panel
Export panel allows to select what artifact are to be exported and in what format. 
When user presses "Export" then a directory select dialog is invoked. 
All selected artifacts are exported there with the file naming scheme:
<dataset_name><artifact_type>.<file_extension>

Pictures are exported as currently seen in the view boxes, i.e. current axis type, scale, units, ranges, etc.

# Export panel layout
- Top: Dropdown list with artifact types. Contains: ".csv", ".png"
- Middle: Scrollable list of available artifacts. Each record has a checkbox to select it. For example if a user selected ".csv" then here he can pick "Average spectrum", "Single spectra from selected files", "Corrected interferograms from selected files". If average is not calculated or no data is selected then these artifact are not shown in the list. 
- Bottom: "Export button" that invokes the directory select dialog and saves all data there.

# Supported artifacts:
- PNG
    - corrected interferograms from selected files
    - uncorrected interferograms from selected files
    - average spectrum
    - spectra from selected files
- CSV
    - corrected interferograms from selected files (x and y axis columns)
    - uncorrected interferograms from selected files (all in single file, separate columns)
    - average spectrum (x column with currently selected unit)
    - spectra from selected files (all in single file, separate columns)
    

