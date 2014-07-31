<?php
###
### Copyright (c) 2011-2013, The Trusted Domain Project.  All rights reserved.
###

#
# PHP code to query a reputation database and generate a reputon
#

#
# load local configuration for databae values
#
require "repute-config.php";

#
# extract query values and build the SQL query
#
if (!isset($_GET["application"]) ||
    !isset($_GET["assertion"]) ||
    !isset($_GET["service"]) ||
    !isset($_GET["subject"]))
	die("Malformed query");

$application = $_GET["application"];
$assertion = $_GET["assertion"];
$service = $_GET["service"];
$subject = $_GET["subject"];

if (strtolower($application) != "email-id")
	die("Unrecognized application");
if (strtolower($assertion) != "spam")
	die("Unrecognized assertion");

if (isset($_GET["reporter"]))
	$reporter = $_GET["reporter"];
else
	$reporter = 0;

if (isset($_GET["format"]))
{
	$format = $_GET["format"];
	if (strtolower($format) != "json")
		die("Unrecognized format");
}

$query1 = "SELECT	ratio_high,
			UNIX_TIMESTAMP(updated),
			rate_samples
           FROM		predictions
           WHERE	name = '$subject'
           AND          reporter = 0";
 
$query2 = "SELECT	daily_limit_low
           FROM		predictions
           WHERE	name = '$subject'
           AND          reporter = $reporter";
 
#
# connect to the DB
#
if (!($connection = new mysqli($repute_db, $repute_user, $repute_pwd, $repute_dbname)))
	die("Unable to connect to database server");

#
# run the first query
#
if (!($result = $connection->query($query1)))
	die("Query failed");

#
# extract results
#
$row = $result->fetch_array(MYSQL_NUM);
if (!$row)
	die("No data available");
$rating = $row[0];
$updated = $row[1];
$samples = $row[2];

#
# run the second query
#
if (!($result = $connection->query($query2)))
	die("Query failed");

$row = $result->fetch_array(MYSQL_NUM);
if (!$row)
	die("No data available");
$rate = $row[0];

#
# MIME header
#

printf("Content-Type: application/reputon+json\n");
printf("\n");

#
# Construct the reputon
#

printf("{\n");
printf("  \"application\": \"email-id\",\n");
printf("  \"reputons\": [\n");
printf("    {\n");
printf("\t\"rater\": \"$service\",\n");
printf("\t\"assertion\": \"spam\",\n");
printf("\t\"rated\": \"$subject\",\n");
printf("\t\"rating\": $rating,\n");
printf("\t\"identity\": \"dkim\",\n");
printf("\t\"rate\": $rate,\n");
printf("\t\"sample-size\": $samples,\n");
printf("\t\"generated\": $updated\n");
printf("    }\n");
printf("  ]\n");
printf("}\n");

# all done!
?>
