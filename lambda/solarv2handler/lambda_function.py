import json
import boto3
from decimal import Decimal

dynamodb = boto3.resource('dynamodb', region_name='ap-southeast-2')
table = dynamodb.Table('paradise-solar-test')


def lambda_handler(event, context):
    try:
        # Parse body
        body = event.get('body', '{}')
        if isinstance(body, str):
            body = json.loads(body)

        # Validate required nested fields
        gateway   = body.get('gateway', {})
        timestamp = body.get('timestamp', {})

        gateway_id  = gateway.get('gateway_id')
        ts_epoch_ms = timestamp.get('ts_epoch_ms')

        if not gateway_id:
            return resp(400, {'error': 'Missing required field: gateway.gateway_id'})
        if ts_epoch_ms is None:
            return resp(400, {'error': 'Missing required field: timestamp.ts_epoch_ms'})

        # Convert floats to Decimal (DynamoDB requirement)
        item = json.loads(json.dumps(body), parse_float=Decimal)

        # Flatten partition key + sort key to top level so DynamoDB table keys work
        item['gateway_id']  = gateway_id
        item['ts_epoch_ms'] = Decimal(str(ts_epoch_ms))

        table.put_item(Item=item)

        return resp(200, {'message': 'OK'})

    except Exception as e:
        print(f'ERROR: {e}')
        return resp(500, {'error': str(e)})


def resp(status, body):
    return {
        'statusCode': status,
        'headers': {
            'Content-Type': 'application/json',
            'Access-Control-Allow-Origin': '*'
        },
        'body': json.dumps(body)
    }
